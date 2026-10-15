/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for gpio_property_logic.hpp — bind/unbind, unknown lines,
 * inject decisions, snapshot no-op merge, and snapshot/retry state machine.
 */

#include "gpio_property_logic.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace pwrseq::gpio_props;

namespace
{

std::unordered_set<std::string> interesting({"PG_LINE", "BC_LINE"});

} // namespace

TEST(GpioPropertyLogic, SnapshotBoundValueInjectsInteresting)
{
    CachedLine cache;
    auto d = applyPropertyUpdate(cache, std::string{"PG_LINE"}, true, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::Inject);
    EXPECT_EQ(d.lineName, "PG_LINE");
    EXPECT_TRUE(d.asserted);
    EXPECT_TRUE(cache.bound);
    EXPECT_TRUE(cache.hasValue);
}

TEST(GpioPropertyLogic, SnapshotUnboundDoesNotInject)
{
    CachedLine cache;
    auto d = applyPropertyUpdate(cache, std::string{"PG_LINE"}, false, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::None);
}

TEST(GpioPropertyLogic, UnknownLineSilent)
{
    CachedLine cache;
    auto d = applyPropertyUpdate(cache, std::string{"ALERT_LINE"}, true, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::None);
    EXPECT_EQ(cache.lineName, "ALERT_LINE");
}

TEST(GpioPropertyLogic, ValueOnlyUpdateUsesCachedLineName)
{
    CachedLine cache;
    cache.lineName = "BC_LINE";
    cache.bound = true;
    cache.hasValue = true;
    cache.value = false;

    auto d = applyPropertyUpdate(cache, std::nullopt, std::nullopt, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::Inject);
    EXPECT_EQ(d.lineName, "BC_LINE");
    EXPECT_TRUE(d.asserted);
}

TEST(GpioPropertyLogic, BindThenValueTwoStep)
{
    CachedLine cache;
    auto d1 = applyPropertyUpdate(cache, std::string{"PG_LINE"}, true,
                                  std::nullopt, interesting);
    EXPECT_EQ(d1.action, Action::None);

    auto d2 = applyPropertyUpdate(cache, std::nullopt, std::nullopt, false,
                                  interesting);
    EXPECT_EQ(d2.action, Action::Inject);
    EXPECT_FALSE(d2.asserted);
}

TEST(GpioPropertyLogic, UnbindStopsInjection)
{
    CachedLine cache;
    ASSERT_EQ(applyPropertyUpdate(cache, std::string{"PG_LINE"}, true, true,
                                  interesting)
                  .action,
              Action::Inject);

    auto d = applyPropertyUpdate(cache, std::nullopt, false, std::nullopt,
                                 interesting);
    EXPECT_EQ(d.action, Action::None);
    EXPECT_FALSE(cache.bound);
    EXPECT_TRUE(cache.hasValue);
}

TEST(GpioPropertyLogic, RebindReinjectsValue)
{
    CachedLine cache;
    applyPropertyUpdate(cache, std::string{"PG_LINE"}, true, true, interesting);
    applyPropertyUpdate(cache, std::nullopt, false, std::nullopt, interesting);

    auto d = applyPropertyUpdate(cache, std::nullopt, true, std::nullopt,
                                 interesting);
    EXPECT_EQ(d.action, Action::Inject);
    EXPECT_TRUE(d.asserted);
}

TEST(GpioPropertyLogic, PathMapOrderingMatchBeforeSnapshot)
{
    std::unordered_map<std::string, CachedLine> byPath;
    const std::string path = "/xyz/openbmc_project/gpio/monitor/lines/PG_LINE";

    auto& early = byPath[path];
    auto race = applyPropertyUpdate(early, std::nullopt, std::nullopt, true,
                                    interesting);
    EXPECT_EQ(race.action, Action::None);

    auto snap = applyPropertyUpdate(byPath[path], std::string{"PG_LINE"}, true,
                                    true, interesting);
    EXPECT_EQ(snap.action, Action::Inject);

    auto edge = applyPropertyUpdate(byPath[path], std::nullopt, std::nullopt,
                                    false, interesting);
    EXPECT_EQ(edge.action, Action::Inject);
    EXPECT_FALSE(edge.asserted);
}

TEST(GpioPropertyLogic, ElectricalSemanticsPreservedForActiveLowPath)
{
    CachedLine cache;
    auto hi = applyPropertyUpdate(cache, std::string{"PG_LINE"}, true, true,
                                  interesting);
    EXPECT_TRUE(hi.asserted);
    auto lo = applyPropertyUpdate(cache, std::nullopt, std::nullopt, false,
                                  interesting);
    EXPECT_FALSE(lo.asserted);
}

TEST(GpioPropertyLogic, EmptyLineNameNeverInjects)
{
    CachedLine cache;
    auto d = applyPropertyUpdate(cache, std::string{""}, true, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::None);
}

TEST(GpioPropertyLogic, DecideInjectRequiresAllFields)
{
    CachedLine incomplete;
    incomplete.lineName = "PG_LINE";
    incomplete.bound = true;
    EXPECT_EQ(decideInject(incomplete, interesting).action, Action::None);

    incomplete.hasValue = true;
    incomplete.value = true;
    auto d = decideInject(incomplete, interesting);
    EXPECT_EQ(d.action, Action::Inject);
    EXPECT_TRUE(d.asserted);
}

// ── Stale-delta / async ordering simulations ─────────────────────────────

TEST(GpioPropertyLogic, AsyncFifoAvoidsStaleReplayAfterSnapshot)
{
    // Models the correct async order: live edges then snapshot reply.
    // Contrast with sync sd_bus_call, which would deliver the same edges
    // *after* the snapshot and replay false→true on top of final true.
    CachedLine cache;
    std::vector<bool> injected;

    auto record = [&](const Decision& d) {
        if (d.action == Action::Inject)
        {
            injected.push_back(d.asserted);
        }
    };

    // Live PropertiesChanged while async GMO is in flight (FIFO before reply).
    record(applyPropertyUpdate(cache, std::string{"PG_LINE"}, true, false,
                               interesting)); // → false
    record(applyPropertyUpdate(cache, std::nullopt, std::nullopt, true,
                               interesting)); // → true

    // Snapshot reply sees final true; no-op because cache already matches.
    record(applySnapshotUpdate(cache, std::string{"PG_LINE"}, true, true,
                               interesting));

    ASSERT_EQ(injected.size(), 2u);
    EXPECT_FALSE(injected[0]);
    EXPECT_TRUE(injected[1]);
    // No third inject — snapshot did not replay stale deltas.
}

TEST(GpioPropertyLogic, SyncReorderHazardWouldReplayStaleDeltas)
{
    // Documents the hazard of applying snapshot first, then queued signals
    // (what sync sd_bus_call does). Same bus events as above end with a
    // false→true replay after the snapshot already seeded true.
    CachedLine cache;
    std::vector<bool> injected;
    auto record = [&](const Decision& d) {
        if (d.action == Action::Inject)
        {
            injected.push_back(d.asserted);
        }
    };

    record(applySnapshotUpdate(cache, std::string{"PG_LINE"}, true, true,
                               interesting)); // snapshot true
    // Queued pre-reply signals applied late:
    record(applyPropertyUpdate(cache, std::nullopt, std::nullopt, false,
                               interesting));
    record(applyPropertyUpdate(cache, std::nullopt, std::nullopt, true,
                               interesting));

    ASSERT_EQ(injected.size(), 3u);
    EXPECT_TRUE(injected[0]);
    EXPECT_FALSE(injected[1]); // stale
    EXPECT_TRUE(injected[2]);
}

TEST(GpioPropertyLogic, SnapshotSeedsWhenSignalsDidNot)
{
    CachedLine cache;
    auto d = applySnapshotUpdate(cache, std::string{"PG_LINE"}, true, true,
                                 interesting);
    EXPECT_EQ(d.action, Action::Inject);
    EXPECT_TRUE(d.asserted);
}

// ── SnapshotMachine ──────────────────────────────────────────────────────

TEST(SnapshotMachine, StartDispatchesCall)
{
    SnapshotMachine m;
    auto a = m.onEvent(SnapshotEvent::StartRequested);
    EXPECT_TRUE(a.dispatchCall);
    m.onEvent(SnapshotEvent::CallDispatched);
    EXPECT_EQ(m.phase, SnapshotPhase::InFlight);
    EXPECT_EQ(m.attempts, 1u);
}

TEST(SnapshotMachine, MissingServiceWaitsWithoutRetryTimer)
{
    SnapshotMachine m;
    m.onEvent(SnapshotEvent::StartRequested);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto a = m.onEvent(SnapshotEvent::CallFailedMissing);
    EXPECT_FALSE(a.armRetryTimer);
    EXPECT_TRUE(a.cancelRetryTimer);
    EXPECT_EQ(m.phase, SnapshotPhase::Idle);
    EXPECT_FALSE(m.serviceOwned);

    auto appear = m.onEvent(SnapshotEvent::ServiceAppeared);
    EXPECT_TRUE(appear.dispatchCall);
    EXPECT_TRUE(m.serviceOwned);
}

TEST(SnapshotMachine, TransientFailureArmsBoundedRetry)
{
    SnapshotMachine m;
    m.serviceOwned = true;
    m.onEvent(SnapshotEvent::StartRequested);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto a = m.onEvent(SnapshotEvent::CallFailedTransient);
    EXPECT_TRUE(a.armRetryTimer);
    EXPECT_EQ(a.retryDelayUsec, SnapshotMachine::kRetryDelayUsec);
    EXPECT_EQ(m.phase, SnapshotPhase::RetryWait);

    auto fired = m.onEvent(SnapshotEvent::RetryTimerFired);
    EXPECT_TRUE(fired.dispatchCall);
}

TEST(SnapshotMachine, ExhaustedRetriesStopArming)
{
    SnapshotMachine m;
    m.maxAttempts = 2;
    m.serviceOwned = true;

    EXPECT_TRUE(m.onEvent(SnapshotEvent::StartRequested).dispatchCall);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto a1 = m.onEvent(SnapshotEvent::CallFailedTransient);
    EXPECT_TRUE(a1.armRetryTimer);
    EXPECT_EQ(m.attempts, 1u);

    EXPECT_TRUE(m.onEvent(SnapshotEvent::RetryTimerFired).dispatchCall);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto a2 = m.onEvent(SnapshotEvent::CallFailedTransient);
    EXPECT_FALSE(a2.armRetryTimer);
    EXPECT_TRUE(a2.cancelRetryTimer);
    EXPECT_EQ(m.attempts, 2u);
    EXPECT_EQ(m.phase, SnapshotPhase::Idle);
}

TEST(SnapshotMachine, SuccessResetsAttempts)
{
    SnapshotMachine m;
    m.onEvent(SnapshotEvent::StartRequested);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto a = m.onEvent(SnapshotEvent::CallSucceeded);
    EXPECT_EQ(m.phase, SnapshotPhase::Ready);
    EXPECT_TRUE(m.everReady);
    EXPECT_EQ(m.attempts, 0u);
    EXPECT_TRUE(a.cancelRetryTimer);
}

TEST(SnapshotMachine, ServiceDepartedClearsAndIdles)
{
    SnapshotMachine m;
    m.onEvent(SnapshotEvent::StartRequested);
    m.onEvent(SnapshotEvent::CallDispatched);
    m.onEvent(SnapshotEvent::CallSucceeded);
    auto a = m.onEvent(SnapshotEvent::ServiceDeparted);
    EXPECT_TRUE(a.clearPathMap);
    EXPECT_TRUE(a.cancelRetryTimer);
    EXPECT_EQ(m.phase, SnapshotPhase::Idle);
    EXPECT_FALSE(m.serviceOwned);
}

TEST(SnapshotMachine, InFlightStartDoesNotDoubleDispatch)
{
    SnapshotMachine m;
    m.onEvent(SnapshotEvent::StartRequested);
    m.onEvent(SnapshotEvent::CallDispatched);
    auto again = m.onEvent(SnapshotEvent::StartRequested);
    EXPECT_FALSE(again.dispatchCall);
    EXPECT_EQ(m.phase, SnapshotPhase::InFlight);
}

TEST(SnapshotMachine, IsServiceMissingError)
{
    EXPECT_TRUE(isServiceMissingError(
        "org.freedesktop.DBus.Error.ServiceUnknown"));
    EXPECT_TRUE(
        isServiceMissingError("org.freedesktop.DBus.Error.NameHasNoOwner"));
    EXPECT_FALSE(isServiceMissingError("org.freedesktop.DBus.Error.Timeout"));
}
