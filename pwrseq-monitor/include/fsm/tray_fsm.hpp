/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "core/types.hpp"
#include "fsm/transition_table.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace pwrseq::fsm
{

/** Per-instance context for the Tray FSM (the single chassis-level FSM).
 *
 *  Tracks the current state of every Module and every FPGA. The Tray itself
 *  has no timer; transitions are driven purely by child-state-changed events
 *  plus operator-driven maintenance toggles.
 */
struct TrayContext
{
    InstanceId                  id;
    std::vector<ModuleState>    moduleStates;   // indexed by module 0..N-1
    std::vector<ElementState>   fpgaStates;     // indexed by FPGA 0..M-1

    std::function<void(TrayState oldState, TrayState newState)>
        onStateChange;
};

/** Tray FSM — one instance per chassis (×1 on LP30).
 *
 *  Five states (T_STBY → T_BOOTING → T_ONLINE / T_FAULT / T_MAINTENANCE).
 *  Aggregator over all Module FSMs and all FPGA Element FSMs. No timers.
 *  Operator-driven maintenance mode toggles via Redfish OEM action.
 *  See design doc §2.3.
 */
class TrayFsm
{
  public:
    /** Construct a Tray FSM with the given number of child modules and FPGAs.
     *  All children start in their respective Stby states.
     */
    TrayFsm(InstanceId trayId, size_t numModules, size_t numFpgas);

    TrayState state() const noexcept { return engine_.currentState(); }

    /** Dispatch a SemanticEvent. Returns true if a row matched and fired
     *  (state may or may not have changed); false if the event was
     *  unmatched (silently ignored by the engine).
     *
     *  Handled kinds:
     *   - EV_MODULE_STATE_CHANGED  (updates moduleStates[child.index])
     *   - EV_FPGA_STATE_CHANGED    (updates fpgaStates[child.index])
     *   - EV_OP_MAINT_ENTER / EXIT (state transitions only)
     *  Other event kinds are silently ignored.
     */
    bool dispatch(const SemanticEvent& ev);

    /** Register a state-change callback. Narrow setter so callers don't
     *  need a non-const ctx() accessor (which would also let them
     *  rewrite moduleStates / fpgaStates and bypass FSM invariants).
     */
    void setOnStateChange(std::function<void(TrayState, TrayState)> cb)
    {
        ctx_.onStateChange = std::move(cb);
    }

    /** Const read-only access to FSM context (for tests / diagnostics). */
    const TrayContext& ctx() const noexcept { return ctx_; }

  private:
    TrayContext                            ctx_;
    FsmEngine<TrayState, TrayContext>      engine_;
};

/** Test/inspection access to the canonical Tray FSM transition table. */
std::span<const TransitionRow<TrayState, TrayContext>> trayFsmTable();

}  // namespace pwrseq::fsm
