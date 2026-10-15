/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "core/types.hpp"
#include "fsm/transition_table.hpp"

#include <array>
#include <functional>
#include <utility>

namespace pwrseq::fsm
{

/** Per-instance context for a Module FSM.
 *
 *  A Module aggregates exactly two child Element FSMs (the two LPUs in the
 *  module, per LP30's design). The Module never observes hardware directly —
 *  it tracks the current state of each child and re-evaluates its own state
 *  on every EV_LPU_STATE_CHANGED.
 */
struct ModuleContext
{
    InstanceId id;                            // this Module's ID
    std::array<InstanceId, 2> childIds;       // the two child Element IDs
    std::array<ElementState, 2> childStates = // current child states
        {ElementState::Stby, ElementState::Stby};

    /** Notify upward (Tray FSM) that this Module's state changed. */
    std::function<void(ModuleState oldState, ModuleState newState)>
        onStateChange;
};

/** Module FSM — one instance per LPU module (8 instances on LP30).
 *
 *  Five states (M_STBY → M_POWERED → M_ONLINE / M_RESETTING / M_FAULT).
 *  Pure aggregator over the two child LPU FSMs. No timers. No DEGRADED state
 *  (any child fault escalates the whole module to M_FAULT). See design doc
 *  §2.2 for the full transition matrix.
 */
class ModuleFsm
{
  public:
    /** Construct a Module FSM owning the two given child Element IDs.
     *  The order of childIds defines which payload maps to "slot 0" vs
     *  "slot 1" — symmetric per §2.2.2 so order doesn't affect outcomes.
     */
    ModuleFsm(InstanceId moduleId, InstanceId childA, InstanceId childB);

    ModuleState state() const noexcept
    {
        return engine_.currentState();
    }

    /** Dispatch a SemanticEvent. Returns true if a row matched and fired
     *  (state may or may not have changed); false if the event was
     *  unmatched (silently ignored by the engine). See
     *  ElementFsm::dispatch for the rationale on returning the outcome.
     *
     *  Only EV_LPU_STATE_CHANGED is consumed; other events are silently
     *  ignored. Before dispatching to the engine, this updates
     *  childStates[] to reflect the event's payload -- so transition
     *  guards see the post-event child-state vector.
     */
    bool dispatch(const SemanticEvent& ev);

    /** Register a state-change callback. Replaces any previously-set
     *  callback. Provided as a narrow setter so callers don't need a
     *  non-const ctx() accessor (which would also let them rewrite
     *  childIds / childStates and break FSM invariants).
     */
    void setOnStateChange(std::function<void(ModuleState, ModuleState)> cb)
    {
        ctx_.onStateChange = std::move(cb);
    }

    /** Const read-only access to FSM context (for tests / diagnostics). */
    const ModuleContext& ctx() const noexcept
    {
        return ctx_;
    }

  private:
    ModuleContext ctx_;
    FsmEngine<ModuleState, ModuleContext> engine_;
};

/** Test/inspection access to the canonical Module FSM transition table. */
std::span<const TransitionRow<ModuleState, ModuleContext>> moduleFsmTable();

} // namespace pwrseq::fsm
