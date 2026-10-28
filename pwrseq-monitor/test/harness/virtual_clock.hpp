/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Manual-tick monotonic clock used by the Tier A test rig (see TEST_PLAN.md).
 * Decouples FSM tests from real wall-clock time so a T_BOOT timeout
 * scenario completes in microseconds instead of 12 seconds.
 *
 * Single-threaded — Tier A runs on one thread by design.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace pwrseq::test
{

class VirtualClock
{
  public:
    using TimerId = uint64_t;

    std::chrono::milliseconds now() const noexcept
    {
        return now_;
    }

    /** Schedule a one-shot callback `delay` ms from now(). Returns a stable
     *  id usable with cancel(). Ids are monotonically increasing and never
     *  reused for the lifetime of this VirtualClock.
     */
    TimerId schedule(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        TimerId id = nextId_++;
        pending_.push_back({id, now_ + delay, std::move(cb), false});
        return id;
    }

    /** Cancel a pending timer. No-op if the timer already fired, was
     *  already cancelled, or the id is unknown.
     */
    void cancel(TimerId id) noexcept
    {
        for (auto& p : pending_)
        {
            if (p.id == id)
            {
                p.cancelled = true;
                return;
            }
        }
    }

    /** Advance the clock by `delta`. Fires every pending timer whose
     *  deadline is <= now() after the advance, in ascending deadline order.
     *  Returns the number of timers fired.
     *
     *  Callbacks MAY schedule new timers. Newly-scheduled timers fire only
     *  on a subsequent advance() — they are not processed recursively here.
     */
    int advance(std::chrono::milliseconds delta)
    {
        now_ += delta;

        // Snapshot the set of timers eligible to fire (deadline crossed
        // and not cancelled) as a list of stable TimerIds, NOT raw
        // pointers into pending_. A callback below is allowed to call
        // schedule(), which does pending_.push_back -- a reallocation
        // would invalidate any held pointers and turn the next loop
        // iteration into a use-after-free. The TimerId is stable for
        // the lifetime of the VirtualClock and lookup is O(N) per
        // dispatched timer, which is fine for the small N (<~10) the
        // FSM tests actually use.
        struct DueEntry
        {
            std::chrono::milliseconds firesAt;
            TimerId id;
        };
        std::vector<DueEntry> due;
        due.reserve(pending_.size());
        for (auto& p : pending_)
        {
            if (!p.cancelled && p.firesAt <= now_)
            {
                due.push_back({p.firesAt, p.id});
            }
        }
        std::sort(due.begin(), due.end(),
                  [](const DueEntry& a, const DueEntry& b) {
                      return a.firesAt < b.firesAt;
                  });

        int fired = 0;
        for (const auto& entry : due)
        {
            // Re-lookup each time -- pending_ may have moved out from
            // under us due to a re-entrant schedule() inside a previous
            // callback. If the entry is gone (couldn't happen with the
            // current API but harmless to guard) or has been cancelled
            // by another callback, skip.
            auto it = std::find_if(
                pending_.begin(), pending_.end(),
                [id = entry.id](const Pending& p) { return p.id == id; });
            if (it == pending_.end() || it->cancelled)
                continue;
            // Mark cancelled BEFORE invoking so the cb can safely re-arm
            // by scheduling a new timer; the slot we just fired will be
            // reaped below. Move the callback out first so re-entrant
            // schedule() can grow pending_ without our local cb being
            // affected by the move.
            it->cancelled = true;
            auto cb = std::move(it->cb);
            cb();
            ++fired;
        }

        // Garbage-collect cancelled / fired entries.
        pending_.erase(
            std::remove_if(pending_.begin(), pending_.end(),
                           [](const Pending& p) { return p.cancelled; }),
            pending_.end());

        return fired;
    }

    /** Number of timers still pending (not cancelled, not yet fired). */
    size_t pendingCount() const noexcept
    {
        size_t n = 0;
        for (const auto& p : pending_)
        {
            if (!p.cancelled)
            {
                ++n;
            }
        }
        return n;
    }

  private:
    struct Pending
    {
        TimerId id;
        std::chrono::milliseconds firesAt;
        std::function<void()> cb;
        bool cancelled;
    };

    std::vector<Pending> pending_;
    std::chrono::milliseconds now_{};
    TimerId nextId_ = 1;
};

} // namespace pwrseq::test
