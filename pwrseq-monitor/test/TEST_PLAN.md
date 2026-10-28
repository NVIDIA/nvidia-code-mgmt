# lpu-pwrseq-monitord — Test Plan

> **Status:** draft. Scenario harness lives under `test/harness/`. A standalone
> runner (`test/harness_runner.cpp`) builds without any openbmc deps so it runs
> on any dev machine.

## 1. Why software-only — and not "ship a test app to the HMC"

The FSM consumes four kinds of input events: `EV_PG_HI/LO`, `EV_BC_HI/LO`,
`EV_RST_ASSERTED/RELEASED`, and `EV_T_BOOT_EXPIRED`. The physical signals that
produce the PG/BC events on LP30 are all **read-only on the BMC side**:

| FSM input | Physical line              | Source                                      | BMC direction |
| --------- | -------------------------- | ------------------------------------------- | ------------- |
| LPU PG    | `LPU${i}_MODULE_PGOOD_R-I` | LPU on-board MCU, packed via LSTP-over-USB  | **read-only** |
| LPU BC    | `LPU${i}_GPIO61-I`         | LPU itself, asserted at `BOOT_BOOTED` (0xF) | **read-only** |
| FPGA PG   | `FPGA_PWR_GD`              | Baseboard combinational aggregate           | **read-only** |
| FPGA BC   | `BB_FPGA_${N}_INIT_DONE_R` | FPGA hard-IP via MCU-M LSTP gpiochip        | **read-only** |

Every line is `-I` from the BMC's perspective — verified against
`meta-nvidia/meta-lp30-vhmc/recipes-kernel/linux/linux-aspeed/eb2_{lpu,bb}_lstp.dtsi`.
There is no way to drive an `EV_PG_LO` from `gpioset` on the BMC. The signals
are sourced by hardware downstream of the BMC; the BMC observes them.

A would-be on-target test app could indirectly stimulate FSM inputs by toggling
BMC-driven outputs (`LPU${i}_MODULE_PWR_EN_R-O`, `LPU${i}_SW_RESET_N-O`,
`BB_FPGA_${N}_RESET_3V3`), but the resulting coverage would be:

- Slow: each scenario takes seconds to minutes of real hardware time.
- Disruptive: actually power-cycles modules.
- Indirect: cannot ergonomically exercise `BOOT_REGRESS`, `PGOOD_DROP`, `T_BOOT`
  expiry without real-time waits, the shared `FPGA_PWR_GD` blast-radius
  scenario, or any multi-element fault.
- Brittle: depends on the LPU OB-MCU and FPGA hard-IP behaving correctly, which
  is what _we're_ trying to test.
- Not portable: needs a lab fixture; can't run in code-CI.

So the canonical way to stimulate the FSM is to **inject `SemanticEvent`s
directly through the same `SemanticEventSink` the real HAL uses**. A scenario
harness that does exactly that is the right test engine. No on-target runner.

## 2. Scenario catalog (initial set)

| ID    | Scenario                                                  | What it exercises                                                                                                                                                    |
| ----- | --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| SC-1  | Happy cold boot — 16 LPUs + 2 FPGAs all assert PG then BC | Tray `Stby → Booting → Online`. Module + tray aggregation under no faults.                                                                                           |
| SC-2  | LPU 5 never asserts BC; `T_BOOT` fires at 12 s            | Element → `Fault(BootTimeout)`; Module 2 → `Fault`; Tray → `Fault`. Single-element fault propagates two tiers.                                                       |
| SC-3  | Late-boot recovery on SC-2's faulted LPU                  | LPU 5 asserts BC after `T_BOOT` already fired. Element/Module/Tray all return to `Online`. Fault-clear propagation.                                                  |
| SC-4  | Shared `FPGA_PWR_GD` drops while both FPGAs Online        | Both FPGA Element FSMs go `Online → Fault(PgoodDrop)` on the same event. Tray → `Fault`. Models the shared-rail blast radius.                                        |
| SC-5  | Redfish reset on a faulted LPU                            | LPU 5 in `Fault` from SC-2; `requestReset(LPU 5)`. FSM transitions `Fault → Resetting → Powered → Online` after BC re-asserts. The new `S_FAULT → S_RESETTING` edge. |
| SC-6  | Reset one LPU; its peer in same module stays Online       | LPU 1 reset; LPU 0 untouched; Module 0 transitions through `Resetting` and back, never `Fault`.                                                                      |
| SC-7  | Boot-regress on a single Online LPU                       | `EV_BC_LO` on an Online LPU. Element → `Fault(BootRegress)`. Module → `Fault`. Tray → `Fault`.                                                                       |
| SC-8  | All-LPUs-fault simultaneously                             | Every LPU's `T_BOOT` expires at once. All 8 Modules and the Tray go `Fault`.                                                                                         |
| SC-9  | PG drop on one LPU while peer is Online                   | `EV_PG_LO` on LPU 1 while LPU 0 is Online. Module 0 → `Fault`. Tray → `Fault`. LPU 0 unaffected.                                                                     |
| SC-10 | `T_BOOT` cancellation by reset before expiry              | LPU 5 in `Powered`. Reset is issued at t=6 s. `T_BOOT` must be cancelled; element must not fault.                                                                    |

SC-10 is the one that catches a real class of bug — a forgotten timer
cancellation. Without a virtual clock we'd never write it because waiting 12 s
of real time is unaffordable in CI.

## 3. Harness architecture (`test/harness/`)

```text
test/harness/
├── virtual_clock.hpp     // header-only manual-tick clock
├── test_rig.hpp          // public API
└── test_rig.cpp          // wires FSMs from a PlatformDescriptor

test/
├── test_scenarios.cpp    // gtest-driven scenario tests (CI)
└── harness_runner.cpp    // standalone main(), prints transitions to stdout
```

Three pieces:

1. **`VirtualClock`** — manual-tick clock used in place of `sd-event`'s real
   timer. Tests advance time explicitly with `clock.advance(t)`; pending timer
   callbacks fire in deadline order during `advance()`. Real time is never
   consumed in tests.
2. **`TestRig`** — owns the FSM hierarchy + `MockDevice` instances + the
   `VirtualClock`. Built from a `PlatformDescriptor` so test topology matches
   production exactly. The rig IS the `SemanticEventSink`; every MockDevice
   routes its events through the rig, which forwards them to the right Element
   FSM and propagates upward.
3. **Scenario tests + runner** — gtests for CI, plus a plain-main() runner that
   prints transitions to stdout so a human can eyeball them on a dev machine.

The rig deliberately does **not** instantiate the production `Orchestrator`. The
orchestrator owns `sd_event*` and `sdbusplus::bus::bus` and is therefore awkward
to test without a real loop and bus. A future iteration can wrap the
orchestrator behind a dependency-injection shim (see §4); for now the rig
duplicates ~150 LOC of wiring (descriptor → FSMs → callbacks → upward
propagation) that mirrors the production code. The duplication is acceptable
because the duplicated code itself is what we want to assert about.

## 4. Future work

| Item                                                                                                                                                                                                           | Why later                                                                                                     |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| **Orchestrator integration test** — instantiate `Orchestrator` with a real (in-process) `sd_event` loop and a stubbed bus; drive `MockDevice`s through its `SemanticEventSink::emit`.                          | Requires plumbing a stub bus; non-trivial. Worth it once the rig has shaken out FSM-level bugs.               |
| **`RecordingReporter`** — captures every Health / State / Conditions[] write the FSMs would push to D-Bus, so scenarios can assert "after this transition, LPU 5's Conditions[] contains a BootTimeout entry." | Today the harness only checks FSM state, which is the most-important invariant. The reporter is a refinement. |
| **JSONL trace replay** — feed a captured trace file (`/var/log/lpu_pwrseq/*.jsonl` from a real run) back through the harness and assert the FSM ends in the recorded state.                                    | Closes the loop from "real run" → "regression test" but needs a trace producer in production first.           |
| **Property-based fuzzing** — random event sequences within the FSM grammar; assert no panic, no impossible state.                                                                                              | High value once the transition tables stabilise.                                                              |

## 5. CI / how to run

```sh
# Via meson (canonical). PWRSEQ_MONITOR_SUPPORT is default-disabled because
# nvidia-code-mgmt ships pwrseq-monitor only on lp30-vhmc; the meson option
# must be enabled explicitly when building tests outside that recipe.
cd nvidia-code-mgmt
meson setup build -Dtests=enabled -DPWRSEQ_MONITOR_SUPPORT=enabled
meson test -C build --suite pwrseq

# Standalone runner (no openbmc / sdbusplus / libgpiod required).
# Run from the nvidia-code-mgmt repo root so source/include paths resolve
# relative to pwrseq-monitor/.
cd nvidia-code-mgmt
g++ -std=c++20 -Ipwrseq-monitor/include -Ipwrseq-monitor/test/harness \
    pwrseq-monitor/src/types.cpp \
    pwrseq-monitor/src/element_fsm.cpp \
    pwrseq-monitor/src/module_fsm.cpp \
    pwrseq-monitor/src/tray_fsm.cpp \
    pwrseq-monitor/src/i_device.cpp \
    pwrseq-monitor/test/harness/test_rig.cpp \
    pwrseq-monitor/test/harness_runner.cpp \
    -o /tmp/pwrseq_runner
/tmp/pwrseq_runner
```

The standalone runner exists precisely so you can iterate on FSM logic in any
C++20 environment — laptop, CI box, sandbox — without dragging in the openbmc
build system.
