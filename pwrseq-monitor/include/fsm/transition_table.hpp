/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "core/types.hpp"

#include <functional>
#include <span>

namespace pwrseq::fsm
{

/** A single row in an FSM transition table.
 *
 *  Both guard and action are optional. A null guard means "always fire".
 *  A null action means "no side effect" (transition still happens).
 *
 *  Side effects happen in the action callback only. The transition itself
 *  is pure (state, event) → state. This lets unit tests assert (a) the
 *  resulting state and (b) the exact action sequence emitted, independent
 *  of any real hardware.
 */
template <typename State, typename Context>
struct TransitionRow
{
    State                                                              from;
    EventKind                                                          event;
    std::function<bool(const Context&, const SemanticEvent&)>          guard;
    State                                                              to;
    std::function<void(Context&, const SemanticEvent&)>                action;
};

/** Generic FSM engine. Walks a static transition table on every event.
 *
 *  If no row in the table matches (state, event), the event is silently
 *  dropped — caller can detect this via the bool return of dispatch().
 *  The design doc treats unmatched events as benign (log + ignore).
 */
template <typename State, typename Context>
class FsmEngine
{
  public:
    using Row = TransitionRow<State, Context>;

    FsmEngine(std::span<const Row> table, State initial) noexcept :
        table_(table), state_(initial)
    {}

    State currentState() const noexcept { return state_; }

    /** Apply event ev to the FSM in context ctx.
     *  Returns true iff a transition row matched and fired.
     */
    bool dispatch(Context& ctx, const SemanticEvent& ev)
    {
        for (const auto& row : table_)
        {
            if (row.from != state_ || row.event != ev.kind)
            {
                continue;
            }
            if (row.guard && !row.guard(ctx, ev))
            {
                continue;
            }
            state_ = row.to;
            if (row.action)
            {
                row.action(ctx, ev);
            }
            return true;
        }
        return false;
    }

  private:
    std::span<const Row> table_;
    State                state_;
};

}  // namespace pwrseq::fsm
