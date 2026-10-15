/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "core/types.hpp"
#include "fsm/transition_table.hpp"

#include <functional>
#include <utility>

namespace pwrseq::fsm
{

/** Per-instance context for an Element FSM (LPU or FPGA).
 *
 *  All side effects of FSM transitions are routed through the function-object
 *  hooks here. The Element FSM itself never touches a timer or an upstream
 *  event sink directly — the owner of the FSM (typically the daemon's main
 *  loop wiring) provides closures that bind these to the real systems.
 *  Unit tests provide no-op hooks (or assertion-recording hooks).
 */
struct ElementContext
{
    InstanceId id;
    FaultCode  faultCode = FaultCode::None;

    /** Start this element's boot-deadline timer (T_BOOT, default 12 s on LP30
     *  but configurable per element class via the descriptor's
     *  init_timeout_ms). Should be a no-op if no timer infrastructure is wired
     *  (e.g. in unit tests).
     */
    std::function<void()> startTBoot;

    /** Cancel the boot-deadline timer if it's currently armed. */
    std::function<void()> cancelTBoot;

    /** Notify the parent FSM (Module for LPUs, Tray for FPGAs) that this
     *  element's state changed. Carries old → new state.
     */
    std::function<void(ElementState oldState, ElementState newState)>
        onStateChange;
};

/** Element FSM — one instance per LPU and per FPGA.
 *
 *  Five states (S_STBY → S_POWERED → S_ONLINE / S_RESETTING / S_FAULT).
 *  See design doc §2.1 for the state diagram and the full transition matrix.
 *
 *  BC level latch (driver layer, see hal/pg_bc_latch.hpp): a BcHi that
 *  arrives in S_STBY is a no-op in this table, but LpuDriver/FpgaDriver keep
 *  the SharedLine cache and synthesize BcHi on the subsequent PgHi when BC
 *  is still asserted. PgLo / reset assert invalidate that cache so a later
 *  PgHi cannot reuse a stale BOOT_COMPLETE from the previous power cycle.
 *
 *  Fault recovery semantics (matters for correctly catching second-cycle boot
 *  failures, see element_fsm.cpp comments for the rationale):
 *
 *   * `BootTimeout` (T_BOOT elapsed; LPU was powered the whole time):
 *     late `EV_BC_HI` returns the FSM directly to S_ONLINE and clears the
 *     fault. The LPU did eventually boot, just slowly.
 *
 *   * `BootRegress` (BC dropped while online without PG dropping): on LP30
 *     the BC line is a clean per-LPU MCU output, so a 1→0→1 sequence means
 *     the MCU watchdog'd and came back. A subsequent `EV_BC_HI` returns
 *     the FSM to S_ONLINE (same row as late BootTimeout recovery). A full
 *     PG cycle or Redfish reset also recovers.
 *
 *   * `PgoodDrop` (PG dropped while online; LPU actually power-cycled):
 *     recovery requires `EV_PG_HI` first → S_POWERED with T_BOOT re-armed,
 *     then `EV_BC_HI` → S_ONLINE. A bare `EV_BC_HI` is ignored because the
 *     LPU's BC level may be stale and the second boot must be re-validated
 *     against T_BOOT. Without this, a hung re-boot would never be detected.
 *     Drivers also invalidate the SharedLine BC cache on PgLo so a later
 *     PgHi cannot synthesize Online from a stale latch.
 */
class ElementFsm
{
  public:
    explicit ElementFsm(InstanceId id);

    /** Current FSM state. */
    ElementState state() const noexcept { return engine_.currentState(); }

    /** Last fault code set on entry to S_FAULT (cleared on recovery).
     *  Meaningful only when state() == ElementState::Fault.
     */
    FaultCode faultCode() const noexcept { return ctx_.faultCode; }

    /** True after the first handled EV_PG_HI (rails seen good at least once).
     *  Used at reset dispatch time to skip never-powered elements (empty module
     *  bays that still wired up at init). */
    bool hasSeenPgHi() const noexcept { return seenPgHi_; }

    /** Dispatch a SemanticEvent. Returns true if a transition row matched
     *  AND fired (state may or may not have changed -- self-transitions
     *  count as fired); false when no row matched the (state, event) pair
     *  (the event was unmatched / "ignored").
     *
     *  Returning the engine outcome lets higher layers (Orchestrator,
     *  test harness) distinguish "this event was handled" from
     *  "this event was dropped on the floor". Looking at FSM state
     *  alone cannot tell the two apart because we have both ignored
     *  events and valid same-state self-transitions.
     *
     *  Caller is responsible for routing events to the right FSM
     *  instance (target check is not enforced here -- that's the
     *  orchestrator's job).
     */
    bool dispatch(const SemanticEvent& ev);

    /** Register the per-element side-effect hooks. Provided as narrow
     *  setters so the FSM context (id, faultCode, child wiring) is not
     *  mutable from outside the FSM machinery.
     */
    void setStartTBoot(std::function<void()> cb)
    {
        ctx_.startTBoot = std::move(cb);
    }
    void setCancelTBoot(std::function<void()> cb)
    {
        ctx_.cancelTBoot = std::move(cb);
    }
    void setOnStateChange(
        std::function<void(ElementState, ElementState)> cb)
    {
        ctx_.onStateChange = std::move(cb);
    }

    /** Const read-only access to FSM context (for tests / diagnostics). */
    const ElementContext& ctx() const noexcept { return ctx_; }

  private:
    ElementContext                              ctx_;
    FsmEngine<ElementState, ElementContext>     engine_;
    bool                                        seenPgHi_ = false;
};

/** Test/inspection access to the canonical Element FSM transition table.
 *  Used by unit tests to enumerate all rows.
 */
std::span<const TransitionRow<ElementState, ElementContext>> elementFsmTable();

}  // namespace pwrseq::fsm
