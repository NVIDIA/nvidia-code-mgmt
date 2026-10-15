/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Module FSM transition table. Mirrors design doc §2.2.2.
 *
 * All transitions fire on EV_LPU_STATE_CHANGED; guards distinguish what to
 * do based on the post-event child-state vector. Order in the table matters
 * — more specific guards come first so the engine matches them before
 * falling through to the catch-alls.
 */

#include "fsm/module_fsm.hpp"

#include <algorithm>
#include <array>

namespace pwrseq::fsm
{

using Row = TransitionRow<ModuleState, ModuleContext>;

namespace
{

// ─── Guard helpers ────────────────────────────────────────────────────────
//
// Two kinds of guards:
//   eventChildIs(S)    — the event's payload was "a child reached state S".
//                        This matches the design's "any child → S_X" rows
//                        (which mean "the child whose state just changed
//                        is now in state X"), not "any child happens to
//                        currently be in state X". Avoids spurious
//                        transitions when a stale child is still in S_X
//                        from before.
//   allChildrenAre(S)  — the post-event child-state vector has all children
//                        in state S. Matches the design's "all children → S".

bool eventChildHasState(const SemanticEvent& ev, ElementState s)
{
    const auto* p = std::get_if<ChildElementPayload>(&ev.payload);
    return p && p->newState == s;
}

bool allChildrenAre(const ModuleContext& ctx, ElementState s)
{
    return std::all_of(ctx.childStates.begin(), ctx.childStates.end(),
                       [s](ElementState x) { return x == s; });
}

// ─── Transition table ────────────────────────────────────────────────────

std::span<const Row> table()
{
    static const auto eventChildPowered = [](const ModuleContext&,
                                             const SemanticEvent& ev) {
        return eventChildHasState(ev, ElementState::Powered);
    };
    static const auto eventChildFault = [](const ModuleContext&,
                                           const SemanticEvent& ev) {
        return eventChildHasState(ev, ElementState::Fault);
    };
    static const auto allOnline = [](const ModuleContext& ctx,
                                     const SemanticEvent&) {
        return allChildrenAre(ctx, ElementState::Online);
    };
    static const auto allResetting = [](const ModuleContext& ctx,
                                        const SemanticEvent&) {
        return allChildrenAre(ctx, ElementState::Resetting);
    };
    static const auto allStby = [](const ModuleContext& ctx,
                                   const SemanticEvent&) {
        return allChildrenAre(ctx, ElementState::Stby);
    };

    static const std::array<Row, 12> kTable = {{
        // ---- from M_STBY ----
        // Any child reached S_POWERED → Module is now powering up.
        {ModuleState::Stby, EventKind::LpuStateChanged, eventChildPowered,
         ModuleState::Powered, nullptr},

        // ---- from M_POWERED ----
        // Any-fault wins over all-online (more specific).
        {ModuleState::Powered, EventKind::LpuStateChanged, eventChildFault,
         ModuleState::Fault, nullptr},
        {ModuleState::Powered, EventKind::LpuStateChanged, allOnline,
         ModuleState::Online, nullptr},

        // ---- from M_ONLINE ----
        // Any child went to fault.
        {ModuleState::Online, EventKind::LpuStateChanged, eventChildFault,
         ModuleState::Fault, nullptr},
        // Mass-reset: both children entered S_RESETTING in parallel.
        {ModuleState::Online, EventKind::LpuStateChanged, allResetting,
         ModuleState::Resetting, nullptr},
        // Per-element warm reset on a 2-child module: one child returns to
        // S_POWERED while the other stays S_ONLINE. The aggregate is no
        // longer fully online -- drop the module to Powered so any boot-
        // complete observer sees the change. This is the "Online +
        // Powered" combo that previously left the module wedged in
        // Online with stale child-state {Online, Powered}.
        {ModuleState::Online, EventKind::LpuStateChanged, eventChildPowered,
         ModuleState::Powered, nullptr},

        // ---- from M_RESETTING ----
        // Any-fault first.
        {ModuleState::Resetting, EventKind::LpuStateChanged, eventChildFault,
         ModuleState::Fault, nullptr},
        // At least one child came back from reset.
        {ModuleState::Resetting, EventKind::LpuStateChanged, eventChildPowered,
         ModuleState::Powered, nullptr},
        // Both went to S_STBY (e.g. power down during reset).
        {ModuleState::Resetting, EventKind::LpuStateChanged, allStby,
         ModuleState::Stby, nullptr},

        // ---- from M_FAULT ----
        // Late-boot recovery: all children eventually online.
        {ModuleState::Fault, EventKind::LpuStateChanged, allOnline,
         ModuleState::Online, nullptr},
        // Operator reset: the just-updated child reached Powered.
        {ModuleState::Fault, EventKind::LpuStateChanged, eventChildPowered,
         ModuleState::Powered, nullptr},
        // Both powered down.
        {ModuleState::Fault, EventKind::LpuStateChanged, allStby,
         ModuleState::Stby, nullptr},
    }};
    return std::span<const Row>(kTable);
}

} // namespace

ModuleFsm::ModuleFsm(InstanceId moduleId, InstanceId childA,
                     InstanceId childB) :
    ctx_{.id = moduleId,
         .childIds = {childA, childB},
         .childStates = {ElementState::Stby, ElementState::Stby},
         .onStateChange = {}},
    engine_(table(), ModuleState::Stby)
{}

bool ModuleFsm::dispatch(const SemanticEvent& ev)
{
    if (ev.kind != EventKind::LpuStateChanged)
    {
        return false;
    }

    const auto* payload = std::get_if<ChildElementPayload>(&ev.payload);
    if (!payload)
    {
        return false; // mis-typed payload; silently drop
    }

    // Locate the slot this child corresponds to and update its state BEFORE
    // engine dispatch so the guards see the post-event state vector.
    int slot = -1;
    for (size_t i = 0; i < ctx_.childIds.size(); ++i)
    {
        if (ctx_.childIds[i] == payload->child)
        {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0)
    {
        return false; // unknown child; not ours
    }
    ctx_.childStates[static_cast<size_t>(slot)] = payload->newState;

    const ModuleState prev = engine_.currentState();
    const bool fired = engine_.dispatch(ctx_, ev);
    if (fired)
    {
        const ModuleState now = engine_.currentState();
        if (now != prev && ctx_.onStateChange)
        {
            ctx_.onStateChange(prev, now);
        }
    }
    return fired;
}

std::span<const TransitionRow<ModuleState, ModuleContext>> moduleFsmTable()
{
    return table();
}

} // namespace pwrseq::fsm
