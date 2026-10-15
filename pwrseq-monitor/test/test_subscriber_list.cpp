/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for hal::common::SubscriberList — the subscriber-list helper
 * that backs SharedLine::dispatch().
 *
 * The helper is the meat of the shared-line plumbing introduced to support
 * boards that wire one GPIO to two logical elements (LP30 EB1 + C00 LPU
 * tray: one MODULE_PGOOD_R for both LPUs on a module). SharedLine itself
 * needs a real libgpiod chip to construct, but the fan-out + RAII semantics
 * live in this header-only helper, so we exercise them here without any
 * hardware dependency.
 */

#include "hal/subscriber_list.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using pwrseq::hal::common::SubscriberList;

namespace
{

TEST(SubscriberList, EmptyByDefault)
{
    SubscriberList sl;
    EXPECT_TRUE(sl.empty());
    EXPECT_EQ(sl.size(), 0u);
    // notify() on an empty list is a no-op.
    sl.notify(true);
    sl.notify(false);
}

TEST(SubscriberList, FansOutInRegistrationOrder)
{
    SubscriberList sl;
    std::vector<std::string> log;
    sl.add([&](bool a) { log.push_back(std::string{"a:"} + (a ? "1" : "0")); });
    sl.add([&](bool a) { log.push_back(std::string{"b:"} + (a ? "1" : "0")); });
    sl.add([&](bool a) { log.push_back(std::string{"c:"} + (a ? "1" : "0")); });

    sl.notify(true);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "a:1");
    EXPECT_EQ(log[1], "b:1");
    EXPECT_EQ(log[2], "c:1");

    log.clear();
    sl.notify(false);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "a:0");
    EXPECT_EQ(log[1], "b:0");
    EXPECT_EQ(log[2], "c:0");
}

TEST(SubscriberList, RemoveByIdSkipsThatSubscriber)
{
    // The shared-module-PG case: LPU0 and LPU1 both subscribed; LPU1's
    // driver shuts down (Subscription dtor calls remove()); LPU0 must still
    // receive subsequent events.
    SubscriberList sl;
    int            a_count = 0;
    int            b_count = 0;
    const auto     a_slot  = sl.add([&](bool) { ++a_count; });
    const auto     b_slot  = sl.add([&](bool) { ++b_count; });

    sl.notify(true);
    EXPECT_EQ(a_count, 1);
    EXPECT_EQ(b_count, 1);

    sl.remove(a_slot);
    EXPECT_EQ(sl.size(), 1u);

    sl.notify(true);
    EXPECT_EQ(a_count, 1);  // unchanged — removed
    EXPECT_EQ(b_count, 2);

    sl.remove(b_slot);
    EXPECT_TRUE(sl.empty());
}

TEST(SubscriberList, RemoveOfUnknownSlotIsIdempotent)
{
    SubscriberList sl;
    int            count = 0;
    const auto     slot  = sl.add([&](bool) { ++count; });

    sl.remove(/*nonexistent*/ 9999);
    EXPECT_EQ(sl.size(), 1u);
    sl.notify(true);
    EXPECT_EQ(count, 1);

    sl.remove(slot);
    sl.remove(slot);  // double-remove — should be a no-op, not a crash
    EXPECT_TRUE(sl.empty());
}

TEST(SubscriberList, SlotIdsAreUniqueAndStable)
{
    SubscriberList sl;
    const auto     s1 = sl.add([](bool) {});
    const auto     s2 = sl.add([](bool) {});
    const auto     s3 = sl.add([](bool) {});
    EXPECT_NE(s1, s2);
    EXPECT_NE(s2, s3);
    EXPECT_NE(s1, s3);

    sl.remove(s2);
    const auto s4 = sl.add([](bool) {});
    // New slot must not collide with the one we just freed (no id reuse;
    // simpler invariant for callers).
    EXPECT_NE(s4, s1);
    EXPECT_NE(s4, s2);
    EXPECT_NE(s4, s3);
}

TEST(SubscriberList, SubscriberExceptionDoesNotBreakChain)
{
    // Without the snapshot + try/catch in notify(), a throwing subscriber
    // would unwind through libgpiod and the sd-event loop. Make sure
    // siblings still run and the iteration completes.
    SubscriberList sl;
    int            before = 0;
    int            after  = 0;
    sl.add([&](bool) { ++before; });
    sl.add([&](bool) { throw std::runtime_error("subscriber blew up"); });
    sl.add([&](bool) { ++after; });

    sl.notify(true);
    EXPECT_EQ(before, 1);
    EXPECT_EQ(after,  1);

    // Non-std exceptions are caught too.
    sl.add([&](bool) { throw 42; });
    sl.add([&](bool) { ++after; });

    sl.notify(false);
    EXPECT_EQ(before, 2);
    EXPECT_EQ(after,  3);
}

TEST(SubscriberList, AddDuringNotifyDoesNotAffectThisRound)
{
    // notify() snapshots subs_ before iterating, so a subscriber that adds
    // a new subscriber mid-flight should not see it called this round.
    // (It WILL be called on subsequent notify()s.)
    SubscriberList sl;
    int            late = 0;
    sl.add([&](bool) {
        sl.add([&](bool) { ++late; });
    });

    sl.notify(true);
    EXPECT_EQ(late, 0);  // newly-added subscriber didn't fire this round

    sl.notify(true);
    EXPECT_EQ(late, 1);  // now it does
}

TEST(SubscriberList, RemoveDuringNotifyIsSafe)
{
    // Symmetric to the add-during-notify case: a subscriber that removes
    // another (or itself) mid-iteration must not crash, thanks to the
    // snapshot. Behaviour: the not-yet-iterated subscriber DOES fire this
    // round (we're iterating the snapshot, not subs_).
    SubscriberList sl;
    int            tail_count = 0;
    std::uint64_t  tail_slot  = 0;

    sl.add([&](bool) {
        sl.remove(tail_slot);
    });
    tail_slot = sl.add([&](bool) { ++tail_count; });

    sl.notify(true);
    EXPECT_EQ(tail_count, 1);  // fired from the snapshot
    EXPECT_EQ(sl.size(), 1u);  // tail removed from live list

    sl.notify(true);
    EXPECT_EQ(tail_count, 1);  // gone for good now
}

}  // namespace
