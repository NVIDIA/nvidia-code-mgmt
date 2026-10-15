/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tray FSM transition table. Mirrors design doc §2.3.2.
 *
 * Uses event-payload-aware guards (eventChild*) to detect "the just-received
 * event was a child reaching state X", and post-event state-vector guards
 * (allModulesOnline, etc.) to detect "all children are now in state X".
 */

#include "fsm/tray_fsm.hpp"

#include <algorithm>
#include <array>

namespace pwrseq::fsm
{

using Row = TransitionRow<TrayState, TrayContext>;

namespace
{

// ─── Guard helpers ────────────────────────────────────────────────────────

bool eventModuleHasState(const SemanticEvent& ev, ModuleState s)
{
    if (ev.kind != EventKind::ModuleStateChanged) return false;
    const auto* p = std::get_if<ChildModulePayload>(&ev.payload);
    return p && p->newState == s;
}

bool eventFpgaHasState(const SemanticEvent& ev, ElementState s)
{
    if (ev.kind != EventKind::FpgaStateChanged) return false;
    const auto* p = std::get_if<ChildElementPayload>(&ev.payload);
    return p && p->newState == s;
}

bool allModulesOnline(const TrayContext& ctx)
{
    return std::all_of(ctx.moduleStates.begin(), ctx.moduleStates.end(),
                       [](ModuleState s) { return s == ModuleState::Online; });
}

bool allFpgasOnline(const TrayContext& ctx)
{
    return std::all_of(ctx.fpgaStates.begin(), ctx.fpgaStates.end(),
                       [](ElementState s) { return s == ElementState::Online; });
}

bool allChildrenOnline(const TrayContext& ctx)
{
    return allModulesOnline(ctx) && allFpgasOnline(ctx);
}

// ─── Transition table ────────────────────────────────────────────────────

std::span<const Row> table()
{
    // "Any child reached Powered" — used for S_STBY → S_BOOTING and
    // S_FAULT → S_BOOTING (operator full reset). Either a module entered
    // M_POWERED or an FPGA entered ElementState::Powered.
    static const auto eventChildPowered = [](const TrayContext&,
                                             const SemanticEvent& ev) {
        return eventModuleHasState(ev, ModuleState::Powered)
            || eventFpgaHasState(ev, ElementState::Powered);
    };

    // "Any child went to fault" — used from S_BOOTING and S_ONLINE.
    static const auto eventChildFault = [](const TrayContext&,
                                           const SemanticEvent& ev) {
        return eventModuleHasState(ev, ModuleState::Fault)
            || eventFpgaHasState(ev, ElementState::Fault);
    };

    // "All children online" — used from S_BOOTING (to S_ONLINE) and
    // from S_FAULT (late recovery to S_ONLINE).
    static const auto allOnline = [](const TrayContext& ctx,
                                     const SemanticEvent&) {
        return allChildrenOnline(ctx);
    };

    // Operator maintenance toggles.
    static const auto always = [](const TrayContext&, const SemanticEvent&) {
        return true;
    };

    static const std::array<Row, 11> kTable = {{
        // ---- from T_STBY ----
        // Any child became Powered → start booting.
        {TrayState::Stby,        EventKind::ModuleStateChanged,
         eventChildPowered,      TrayState::Booting,     nullptr},
        {TrayState::Stby,        EventKind::FpgaStateChanged,
         eventChildPowered,      TrayState::Booting,     nullptr},

        // ---- from T_BOOTING ----
        // Any-fault wins; then check all-online.
        {TrayState::Booting,     EventKind::ModuleStateChanged,
         eventChildFault,        TrayState::Fault,       nullptr},
        {TrayState::Booting,     EventKind::FpgaStateChanged,
         eventChildFault,        TrayState::Fault,       nullptr},
        {TrayState::Booting,     EventKind::ModuleStateChanged,
         allOnline,              TrayState::Online,      nullptr},
        {TrayState::Booting,     EventKind::FpgaStateChanged,
         allOnline,              TrayState::Online,      nullptr},

        // ---- from T_ONLINE ----
        {TrayState::Online,      EventKind::ModuleStateChanged,
         eventChildFault,        TrayState::Fault,       nullptr},
        {TrayState::Online,      EventKind::FpgaStateChanged,
         eventChildFault,        TrayState::Fault,       nullptr},
        {TrayState::Online,      EventKind::OpMaintenanceEnter,
         always,                 TrayState::Maintenance, nullptr},

        // ---- from T_FAULT ----
        // Late recovery: all children online again.
        {TrayState::Fault,       EventKind::ModuleStateChanged,
         allOnline,              TrayState::Online,      nullptr},
        {TrayState::Fault,       EventKind::FpgaStateChanged,
         allOnline,              TrayState::Online,      nullptr},
    }};
    // Note: there's also "operator full reset (any child Powered) → Booting"
    // and "maintenance exit" — kept out of the main array above to keep it
    // tidy; appended below via a static aggregate at first call.
    // Actually we add them properly inline:

    // Guards for the maintenance-exit rows. dispatch() still updates the
    // child-state vectors during T_MAINTENANCE (the entry/exit events
    // themselves don't carry a child payload), so by the time
    // OpMaintenanceExit fires the tracked vectors reflect any faults that
    // occurred during the maintenance window. Pick the exit state from
    // those tracked vectors rather than unconditionally returning to
    // Online -- otherwise OpMaintenanceExit would clear any genuine
    // hardware fault that happened while maintenance was active.
    static const auto anyChildFault = [](const TrayContext& ctx,
                                         const SemanticEvent&) {
        return std::any_of(ctx.moduleStates.begin(), ctx.moduleStates.end(),
                           [](ModuleState s) { return s == ModuleState::Fault; })
            || std::any_of(ctx.fpgaStates.begin(), ctx.fpgaStates.end(),
                           [](ElementState s) { return s == ElementState::Fault; });
    };
    static const auto allChildrenOnlineGuard =
        [](const TrayContext& ctx, const SemanticEvent&) {
            return allChildrenOnline(ctx);
        };
    // Fallback for the "mixed booting/powered/stby" cases -- if not
    // fault, not all-online, we land in Booting and let the normal
    // child-state-change rows resolve to Online or Fault from there.
    // FsmEngine is first-match-wins, so this third row is reached only
    // when the two more-specific guards above fail.

    static const std::array<Row, 16> kFullTable = {{
        kTable[0], kTable[1], kTable[2], kTable[3], kTable[4], kTable[5],
        kTable[6], kTable[7], kTable[8], kTable[9], kTable[10],

        // T_FAULT + any child Powered (operator full reset) → T_BOOTING
        {TrayState::Fault,       EventKind::ModuleStateChanged,
         eventChildPowered,      TrayState::Booting,     nullptr},
        {TrayState::Fault,       EventKind::FpgaStateChanged,
         eventChildPowered,      TrayState::Booting,     nullptr},

        // T_MAINTENANCE + EV_OP_MAINT_EXIT: pick exit state from tracked
        // child states. First-match-wins, most-specific first:
        //   any child in Fault       -> exit to Fault
        //   else all children Online -> exit to Online
        //   otherwise (mixed)        -> exit to Booting and let normal
        //                               child-state rows resolve from there
        {TrayState::Maintenance, EventKind::OpMaintenanceExit,
         anyChildFault,          TrayState::Fault,       nullptr},
        {TrayState::Maintenance, EventKind::OpMaintenanceExit,
         allChildrenOnlineGuard, TrayState::Online,      nullptr},
        {TrayState::Maintenance, EventKind::OpMaintenanceExit,
         always,                 TrayState::Booting,     nullptr},
    }};

    return std::span<const Row>(kFullTable);
}

}  // namespace

TrayFsm::TrayFsm(InstanceId trayId, size_t numModules, size_t numFpgas) :
    ctx_{.id = trayId,
         .moduleStates = std::vector<ModuleState>(numModules, ModuleState::Stby),
         .fpgaStates = std::vector<ElementState>(numFpgas, ElementState::Stby),
         .onStateChange = {}},
    engine_(table(), TrayState::Stby)
{}

bool TrayFsm::dispatch(const SemanticEvent& ev)
{
    // Update tracked child state vectors first, so guards see post-event values.
    switch (ev.kind)
    {
        case EventKind::ModuleStateChanged:
        {
            const auto* p = std::get_if<ChildModulePayload>(&ev.payload);
            if (p && p->child.index < ctx_.moduleStates.size())
            {
                ctx_.moduleStates[p->child.index] = p->newState;
            }
            else
            {
                return false;  // missing or out-of-range payload
            }
            break;
        }
        case EventKind::FpgaStateChanged:
        {
            const auto* p = std::get_if<ChildElementPayload>(&ev.payload);
            if (p && p->child.index < ctx_.fpgaStates.size())
            {
                ctx_.fpgaStates[p->child.index] = p->newState;
            }
            else
            {
                return false;
            }
            break;
        }
        case EventKind::OpMaintenanceEnter:
        case EventKind::OpMaintenanceExit:
            break;  // pass through to engine
        default:
            return false;  // ignore unrelated event kinds
    }

    const TrayState prev = engine_.currentState();
    const bool fired = engine_.dispatch(ctx_, ev);
    if (fired)
    {
        const TrayState now = engine_.currentState();
        if (now != prev && ctx_.onStateChange)
        {
            ctx_.onStateChange(prev, now);
        }
    }
    return fired;
}

std::span<const TransitionRow<TrayState, TrayContext>> trayFsmTable()
{
    return table();
}

}  // namespace pwrseq::fsm
