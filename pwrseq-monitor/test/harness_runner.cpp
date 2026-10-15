/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Standalone scenario runner for the Tier A test rig. No gtest, no
 * openbmc, no sdbusplus, no libgpiod. Compile and run on any C++20 host:
 *
 *   g++ -std=c++20 -Iinclude -Itest/harness \
 *       src/types.cpp src/element_fsm.cpp src/module_fsm.cpp \
 *       src/tray_fsm.cpp src/i_device.cpp \
 *       test/harness/test_rig.cpp test/harness_runner.cpp \
 *       -o /tmp/pwrseq_runner
 *   /tmp/pwrseq_runner
 *
 * Prints every FSM state transition to stdout so you can eyeball the
 * propagation while iterating on FSM tables.
 */

#include "config/descriptor.hpp"
#include "core/types.hpp"
#include "harness/test_rig.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pwrseq;
using namespace pwrseq::test;
using std::chrono::milliseconds;

// ─── small pretty-printers ──────────────────────────────────────────────────

static const char* tierName(TransitionRecord::Tier t)
{
    switch (t)
    {
        case TransitionRecord::Tier::Element: return "ELEM";
        case TransitionRecord::Tier::Module:  return "MOD ";
        case TransitionRecord::Tier::Tray:    return "TRAY";
    }
    return "?   ";
}

static const char* stateNameElement(int v)
{
    return to_string(static_cast<ElementState>(v)).data();
}
static const char* stateNameModule(int v)
{
    return to_string(static_cast<ModuleState>(v)).data();
}
static const char* stateNameTray(int v)
{
    return to_string(static_cast<TrayState>(v)).data();
}

static const char* (*stateNameFor(TransitionRecord::Tier t))(int)
{
    switch (t)
    {
        case TransitionRecord::Tier::Element: return stateNameElement;
        case TransitionRecord::Tier::Module:  return stateNameModule;
        case TransitionRecord::Tier::Tray:    return stateNameTray;
    }
    return stateNameElement;
}

static void dumpNewTransitions(const std::vector<TransitionRecord>& trace,
                               size_t& cursor)
{
    for (; cursor < trace.size(); ++cursor)
    {
        const auto& r = trace[cursor];
        auto* nameFn  = stateNameFor(r.tier);
        std::printf("    t=%6lldms  %s  %-22s  %s -> %s\n",
                    static_cast<long long>(r.at.count()),
                    tierName(r.tier),
                    to_string(r.who).c_str(),
                    nameFn(r.fromState), nameFn(r.toState));
    }
}

static void banner(const char* title)
{
    std::printf("\n");
    std::printf("════════════════════════════════════════════════════════════"
                "════════════════\n");
    std::printf("  %s\n", title);
    std::printf("────────────────────────────────────────────────────────────"
                "────────────────\n");
}

// ─── build a representative LP30 descriptor in memory ──────────────────────

static config::PlatformDescriptor makeLp30Descriptor()
{
    config::PlatformDescriptor d;
    d.platform   = "LP30-EB2 (test rig)";
    d.chassisId  = "HGX_Chassis_0";
    d.tBootMs    = milliseconds(12'000);

    // 8 modules, each with 2 LPUs.
    for (int m = 0; m < 8; ++m)
    {
        config::ModuleDescriptor mod;
        mod.id = "HGX_LPU_Module_" + std::to_string(m);
        for (int k = 0; k < 2; ++k)
        {
            config::ElementDescriptor e;
            e.kind = config::ElementDescriptor::Kind::Lpu;
            e.id   = "HGX_LPU_" + std::to_string(m * 2 + k);
            e.pg.line       = "LPU" + std::to_string(m * 2 + k + 1) +
                              "_MODULE_PGOOD_R-I";
            e.bc.line       = "LPU" + std::to_string(m * 2 + k + 1) +
                              "_GPIO61-I";
            e.resetLine.line = "LPU" + std::to_string(m * 2 + k + 1) +
                               "_SW_RESET_N-O";
            mod.elements.push_back(std::move(e));
        }
        d.modules.push_back(std::move(mod));
    }

    // 2 FPGAs.
    {
        config::FpgaDescriptor f;
        f.id          = "HGX_FPGA_W";
        f.pg.line     = "FPGA_PWR_GD";
        f.bc.line     = "BB_FPGA_0_INIT_DONE_R";
        f.resetLine.line = "BB_FPGA_0_RESET_3V3-O";
        d.fpgas.push_back(std::move(f));
    }
    {
        config::FpgaDescriptor f;
        f.id          = "HGX_FPGA_E";
        f.pg.line     = "FPGA_PWR_GD";
        f.bc.line     = "BB_FPGA_1_INIT_DONE_R";
        f.resetLine.line = "BB_FPGA_1_RESET_3V3-O";
        d.fpgas.push_back(std::move(f));
    }

    return d;
}

// ─── assertion helpers (we don't drag in gtest here) ───────────────────────

static int g_failures = 0;
static int g_checks   = 0;

#define EXPECT_EQ_INT(actual, expected, label)                                 \
    do                                                                         \
    {                                                                          \
        ++g_checks;                                                            \
        auto __a = (actual);                                                   \
        auto __e = (expected);                                                 \
        if (__a != __e)                                                        \
        {                                                                      \
            ++g_failures;                                                      \
            std::printf("    ✗ %s: expected %d, got %d\n", (label),            \
                        static_cast<int>(__e), static_cast<int>(__a));         \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            std::printf("    ✓ %s\n", (label));                                \
        }                                                                      \
    } while (0)

#define EXPECT_ELEMENT(rig, id, st, label)                                     \
    EXPECT_EQ_INT(static_cast<int>((rig).elementState(id)),                    \
                  static_cast<int>(st), label)

#define EXPECT_MODULE(rig, id, st, label)                                      \
    EXPECT_EQ_INT(static_cast<int>((rig).moduleState(id)),                     \
                  static_cast<int>(st), label)

#define EXPECT_TRAY(rig, st, label)                                            \
    EXPECT_EQ_INT(static_cast<int>((rig).trayState()),                         \
                  static_cast<int>(st), label)

#define EXPECT_FAULT(rig, id, fc, label)                                       \
    EXPECT_EQ_INT(static_cast<int>((rig).elementFault(id)),                    \
                  static_cast<int>(fc), label)

// ─── scenarios ─────────────────────────────────────────────────────────────

static void sc1_happy_cold_boot(TestRig& rig)
{
    banner("SC-1: Happy cold boot — 16 LPUs + 2 FPGAs assert PG then BC");

    size_t cursor = rig.trace().size();

    // Phase 1 — PG asserted on every element. Each element should go
    // Stby → Powered, T_BOOT armed. Modules go Stby → Powered as both
    // children become Powered. Tray goes Stby → Booting.
    std::printf("  → injecting PgHi on all 16 LPUs and 2 FPGAs\n");
    rig.injectAllLpus(EventKind::PgHi);
    rig.injectAllFpgas(EventKind::PgHi);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_TRAY(rig, TrayState::Booting, "tray=Booting after PG on all");
    EXPECT_ELEMENT(rig, TestRig::lpuId(1),  ElementState::Powered, "LPU1=Powered");
    EXPECT_MODULE(rig,  TestRig::moduleId(0), ModuleState::Powered, "Module0=Powered");
    EXPECT_EQ_INT(rig.pendingTimers(), 18u, "18 T_BOOT timers armed");

    // Phase 2 — BC asserted on every element. Each Element → Online,
    // each Module → Online, Tray → Online.
    std::printf("  → injecting BcHi on all 16 LPUs and 2 FPGAs\n");
    rig.injectAllLpus(EventKind::BcHi);
    rig.injectAllFpgas(EventKind::BcHi);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_TRAY(rig, TrayState::Online, "tray=Online after BC on all");
    EXPECT_ELEMENT(rig, TestRig::lpuId(7),  ElementState::Online, "LPU7=Online");
    EXPECT_ELEMENT(rig, TestRig::fpgaId(0), ElementState::Online, "FPGA-W=Online");
    EXPECT_MODULE(rig,  TestRig::moduleId(3), ModuleState::Online, "Module3=Online");
    EXPECT_EQ_INT(rig.pendingTimers(), 0u, "all T_BOOT timers cancelled");
}

static void sc2_lpu5_boot_timeout(TestRig& rig)
{
    banner("SC-2: LPU 5 never asserts BC; T_BOOT expires at 12 s");

    size_t cursor = rig.trace().size();
    auto lpu5 = TestRig::lpuId(5);

    // Bring everything up but skip LPU 5's BcHi.
    std::printf("  → cold-booting all except LPU 5 (no BcHi for LPU 5)\n");
    rig.injectAllLpus(EventKind::PgHi);
    rig.injectAllFpgas(EventKind::PgHi);
    for (int i = 1; i <= 16; ++i)
    {
        if (i == 5) continue;
        rig.inject(TestRig::lpuId(i), EventKind::BcHi);
    }
    rig.injectAllFpgas(EventKind::BcHi);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered,
                   "LPU5 still Powered (waiting for BC)");
    EXPECT_TRAY(rig, TrayState::Booting,
                "tray still Booting (LPU 5 not Online)");

    // Now tick past the 12 s T_BOOT deadline.
    std::printf("  → ticking +12000ms (T_BOOT)\n");
    int fired = rig.tick(milliseconds(12'000));
    std::printf("    %d timer(s) fired\n", fired);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_EQ_INT(fired, 1, "exactly one timer fired");
    EXPECT_ELEMENT(rig, lpu5, ElementState::Fault, "LPU5=Fault");
    EXPECT_FAULT(rig, lpu5, FaultCode::BootTimeout, "LPU5 fault=BootTimeout");
    EXPECT_MODULE(rig, TestRig::moduleId(2), ModuleState::Fault,
                  "Module2=Fault (LPU5 lives there)");
    EXPECT_TRAY(rig, TrayState::Fault, "tray=Fault");
}

static void sc3_lpu5_late_boot_recovery(TestRig& rig)
{
    banner("SC-3: Late-boot recovery on LPU 5 (already faulted)");

    size_t cursor = rig.trace().size();
    auto lpu5 = TestRig::lpuId(5);

    EXPECT_ELEMENT(rig, lpu5, ElementState::Fault, "pre: LPU5=Fault");

    std::printf("  → LPU 5 finally asserts BC, post-timeout\n");
    rig.inject(lpu5, EventKind::BcHi);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_ELEMENT(rig, lpu5, ElementState::Online, "LPU5=Online");
    EXPECT_FAULT(rig, lpu5, FaultCode::None, "LPU5 fault cleared");
    EXPECT_MODULE(rig, TestRig::moduleId(2), ModuleState::Online,
                  "Module2=Online (both children Online)");
    EXPECT_TRAY(rig, TrayState::Online, "tray=Online");
}

static void sc4_fpga_pgood_blast_radius(TestRig& rig)
{
    banner("SC-4: Shared FPGA_PWR_GD drops; both FPGAs go Fault(PgoodDrop)");

    size_t cursor = rig.trace().size();
    auto fW = TestRig::fpgaId(0);
    auto fE = TestRig::fpgaId(1);

    EXPECT_ELEMENT(rig, fW, ElementState::Online, "pre: FPGA-W=Online");
    EXPECT_ELEMENT(rig, fE, ElementState::Online, "pre: FPGA-E=Online");

    std::printf("  → emulate FPGA_PWR_GD drop (single shared rail)\n");
    rig.inject(fW, EventKind::PgLo);
    rig.inject(fE, EventKind::PgLo);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_ELEMENT(rig, fW, ElementState::Fault, "FPGA-W=Fault");
    EXPECT_ELEMENT(rig, fE, ElementState::Fault, "FPGA-E=Fault");
    EXPECT_FAULT(rig, fW, FaultCode::PgoodDrop, "FPGA-W fault=PgoodDrop");
    EXPECT_FAULT(rig, fE, FaultCode::PgoodDrop, "FPGA-E fault=PgoodDrop");
    EXPECT_TRAY(rig, TrayState::Fault, "tray=Fault");
}

static void sc5_reset_on_faulted_lpu()
{
    // Standalone scenario — bring up its own rig.
    banner("SC-5: Redfish reset on faulted LPU 5 (Fault → Resetting → Online)");

    TestRig rig(makeLp30Descriptor());
    size_t cursor = 0;

    // Cold-boot everything except LPU 5.
    rig.injectAllLpus(EventKind::PgHi);
    rig.injectAllFpgas(EventKind::PgHi);
    for (int i = 1; i <= 16; ++i)
        if (i != 5) rig.inject(TestRig::lpuId(i), EventKind::BcHi);
    rig.injectAllFpgas(EventKind::BcHi);
    rig.tick(milliseconds(12'000));
    cursor = rig.trace().size();

    auto lpu5 = TestRig::lpuId(5);
    EXPECT_ELEMENT(rig, lpu5, ElementState::Fault, "pre: LPU5=Fault");

    std::printf("  → requestReset(LPU 5)\n");
    rig.requestReset(lpu5);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered,
                   "LPU5=Powered after reset release");
    EXPECT_EQ_INT(rig.resetCallCount(lpu5), 1, "device.reset() called once");
    EXPECT_FAULT(rig, lpu5, FaultCode::None, "fault cleared on reset");

    std::printf("  → LPU 5 finishes booting (BcHi)\n");
    rig.inject(lpu5, EventKind::BcHi);
    dumpNewTransitions(rig.trace(), cursor);

    EXPECT_ELEMENT(rig, lpu5, ElementState::Online, "LPU5=Online post-boot");
    EXPECT_TRAY(rig, TrayState::Online, "tray=Online");
}

static void sc10_reset_cancels_tboot()
{
    banner("SC-10: Reset at t=6s cancels T_BOOT; element must NOT fault");

    TestRig rig(makeLp30Descriptor());
    auto lpu5 = TestRig::lpuId(5);

    rig.inject(lpu5, EventKind::PgHi);  // → Powered, T_BOOT armed
    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered, "post-PG: Powered");
    EXPECT_EQ_INT(rig.pendingTimers(), 1u, "1 T_BOOT timer armed");

    std::printf("  → +6000ms (halfway to T_BOOT)\n");
    rig.tick(milliseconds(6'000));
    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered, "still Powered at t=6s");

    std::printf("  → requestReset(LPU 5) at t=6s\n");
    rig.requestReset(lpu5);
    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered,
                   "after reset: Powered again, new T_BOOT armed");
    EXPECT_EQ_INT(rig.pendingTimers(), 1u, "exactly 1 timer (old cancelled)");

    std::printf("  → +6000ms (would have been original T_BOOT deadline)\n");
    int fired = rig.tick(milliseconds(6'000));
    EXPECT_EQ_INT(fired, 0, "original T_BOOT did NOT fire");
    EXPECT_ELEMENT(rig, lpu5, ElementState::Powered, "still Powered (no fault)");

    std::printf("  → +6000ms more (now past the *new* T_BOOT)\n");
    fired = rig.tick(milliseconds(6'000));
    EXPECT_EQ_INT(fired, 1, "new T_BOOT fired");
    EXPECT_ELEMENT(rig, lpu5, ElementState::Fault, "now Fault (BootTimeout)");
    EXPECT_FAULT(rig, lpu5, FaultCode::BootTimeout, "fault=BootTimeout");
}

// ─── main ──────────────────────────────────────────────────────────────────

int main()
{
    std::printf("lpu-pwrseq-monitord — Tier A scenario runner\n");
    std::printf("topology: 16 LPUs in 8 modules + 2 FPGAs; T_BOOT=12000ms\n");

    {
        TestRig rig(makeLp30Descriptor());
        sc1_happy_cold_boot(rig);
        // Carry rig state forward across SC-2 / SC-3 / SC-4 so we exercise
        // cumulative state transitions through one "system lifetime".
        // (SC-2 will produce its own cold-boot at the start; reset rig to
        // get a clean baseline.)
    }
    {
        TestRig rig(makeLp30Descriptor());
        sc2_lpu5_boot_timeout(rig);
        sc3_lpu5_late_boot_recovery(rig);
        sc4_fpga_pgood_blast_radius(rig);
    }
    sc5_reset_on_faulted_lpu();
    sc10_reset_cancels_tboot();

    std::printf("\n");
    std::printf("════════════════════════════════════════════════════════════"
                "════════════════\n");
    std::printf("  %d checks, %d failure(s)\n", g_checks, g_failures);
    std::printf("════════════════════════════════════════════════════════════"
                "════════════════\n");

    return g_failures == 0 ? 0 : 1;
}
