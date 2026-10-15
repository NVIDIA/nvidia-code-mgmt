/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace pwrseq::hal::common
{

/** Subscriber callback: invoked once per drained edge event with the new
 *  logically-asserted state. Defined here (rather than in driver_common.hpp)
 *  so SubscriberList can stay free of libgpiod / phosphor-logging deps and
 *  remain unit-testable in isolation.
 */
using LineSubscriber = std::function<void(bool asserted)>;

/** Subscriber-list helper. Pure bookkeeping — no libgpiod / openbmc deps.
 *  Header-only so it can be exercised in unit tests without linking the
 *  daemon's HAL TU (which pulls libgpiod, sdbusplus, phosphor-logging).
 *
 *  Single-thread use only — designed for the daemon's sd-event loop.
 */
class SubscriberList
{
  public:
    /** Register a subscriber. Returns a stable slot id used by remove(). */
    std::uint64_t add(LineSubscriber cb)
    {
        const std::uint64_t slot = nextSlot_++;
        subs_.push_back(Slot{slot, std::move(cb)});
        return slot;
    }

    /** Remove a subscriber by slot. Idempotent — unknown slots are ignored. */
    void remove(std::uint64_t slot) noexcept
    {
        auto it = std::find_if(subs_.begin(), subs_.end(),
                               [slot](const Slot& s) { return s.id == slot; });
        if (it != subs_.end())
        {
            subs_.erase(it);
        }
    }

    /** Invoke each live subscriber with `asserted`, in registration order.
     *
     *  Snapshots the subscriber vector before iteration so a callback that
     *  itself subscribes/unsubscribes cannot invalidate the loop. (Single-
     *  threaded code; the snapshot is purely for iterator stability.)
     *
     *  Subscriber exceptions are caught and reported on stderr, then
     *  swallowed, so one bad subscriber cannot break the rest of the chain
     *  or unwind through libgpiod / sd-event.
     */
    void notify(bool asserted) const
    {
        const auto snapshot = subs_;
        for (const auto& s : snapshot)
        {
            try
            {
                s.cb(asserted);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr,
                             "SubscriberList: subscriber %llu threw: %s\n",
                             static_cast<unsigned long long>(s.id), e.what());
            }
            catch (...)
            {
                std::fprintf(stderr,
                             "SubscriberList: subscriber %llu threw "
                             "non-std exception\n",
                             static_cast<unsigned long long>(s.id));
            }
        }
    }

    std::size_t size()  const noexcept { return subs_.size(); }
    bool        empty() const noexcept { return subs_.empty(); }

  private:
    struct Slot { std::uint64_t id; LineSubscriber cb; };
    std::vector<Slot> subs_;
    std::uint64_t     nextSlot_ = 1;
};

}  // namespace pwrseq::hal::common
