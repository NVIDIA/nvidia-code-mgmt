/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Pure helpers for xyz.openbmc_project.Gpio.Monitor.Line property
 * consumption. Kept free of sd-bus so unit tests can cover bind/unbind,
 * unknown-line filtering, inject decisions, and snapshot/retry ordering
 * without a D-Bus daemon.
 *
 * Ordering note (why async GetManagedObjects):
 *   sd_bus_call() queues unrelated incoming messages (including match
 *   signals) until the method reply is seen, then returns; those signals
 *   are only dispatched after the caller returns to the event loop
 *   (Lennart Poettering, systemd-devel 2019-01). That reorders
 *   PropertiesChanged that raced the call to *after* the snapshot apply,
 *   which can replay stale Value deltas (e.g. false then true) even when
 *   the snapshot already has the final true. Async sd_bus_call_async keeps
 *   FIFO order with match callbacks, so live edges and the snapshot reply
 *   cannot invert.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pwrseq::gpio_props
{

/** Cached view of one Line object (keyed by D-Bus object path elsewhere). */
struct CachedLine
{
    std::string lineName;
    bool bound = false;
    bool hasValue = false;
    /** Electrical level; same semantics as ReadGpio / SharedLine::notifyExternal. */
    bool value = false;
};

enum class Action
{
    None,
    Inject,
};

struct Decision
{
    Action action = Action::None;
    std::string lineName;
    bool asserted = false;
};

/** True when lineName is a power-sequence input we care about. */
inline bool isInteresting(const std::string& lineName,
                          const std::unordered_set<std::string>& interesting)
{
    return !lineName.empty() && interesting.count(lineName) != 0;
}

/** Decide whether a fully-known cache entry should be injected. */
inline Decision decideInject(
    const CachedLine& cache,
    const std::unordered_set<std::string>& interesting) noexcept
{
    if (!cache.bound || !cache.hasValue ||
        !isInteresting(cache.lineName, interesting))
    {
        return {};
    }
    return Decision{Action::Inject, cache.lineName, cache.value};
}

/** Merge a PropertiesChanged / InterfacesAdded property delta into cache.
 *
 *  Missing optionals mean "property not present in this update".
 *  Returns an Inject decision only when Bound && Value are known and the
 *  LineName is in the interesting set. Unbound / unknown lines yield None.
 */
inline Decision applyPropertyUpdate(
    CachedLine& cache,
    const std::optional<std::string>& lineNameUpdate,
    const std::optional<bool>& boundUpdate,
    const std::optional<bool>& valueUpdate,
    const std::unordered_set<std::string>& interesting)
{
    if (lineNameUpdate && !lineNameUpdate->empty())
    {
        cache.lineName = *lineNameUpdate;
    }
    if (boundUpdate)
    {
        cache.bound = *boundUpdate;
    }
    if (valueUpdate)
    {
        cache.value = *valueUpdate;
        cache.hasValue = true;
    }

    return decideInject(cache, interesting);
}

/** Apply a GetManagedObjects row; suppress no-op injects when cache already
 *  matches (signals may have seeded the line before the async reply).
 */
inline Decision applySnapshotUpdate(
    CachedLine& cache,
    const std::optional<std::string>& lineNameUpdate,
    const std::optional<bool>& boundUpdate,
    const std::optional<bool>& valueUpdate,
    const std::unordered_set<std::string>& interesting)
{
    const bool prevHas = cache.hasValue;
    const bool prevBound = cache.bound;
    const bool prevVal = cache.value;

    auto d = applyPropertyUpdate(cache, lineNameUpdate, boundUpdate,
                                 valueUpdate, interesting);
    if (d.action != Action::Inject)
    {
        return d;
    }
    if (prevHas && prevBound && cache.bound && prevVal == cache.value)
    {
        return {};
    }
    return d;
}

// ── Snapshot / retry state machine ───────────────────────────────────────

enum class SnapshotPhase
{
    Idle,          // need (re)start
    InFlight,      // async GetManagedObjects outstanding
    Ready,         // at least one successful snapshot
    RetryWait,     // transient failure; timer armed
};

enum class SnapshotEvent
{
    StartRequested,         // ctor / explicit resnapshot
    CallDispatched,         // async call slot created
    CallSucceeded,
    CallFailedMissing,      // service not on bus
    CallFailedTransient,    // timeout / other; name may still be owned
    RetryTimerFired,
    ServiceAppeared,
    ServiceDeparted,
};

struct SnapshotActions
{
    bool dispatchCall = false;
    bool armRetryTimer = false;
    bool cancelRetryTimer = false;
    bool clearPathMap = false;
    uint64_t retryDelayUsec = 0;
};

struct SnapshotMachine
{
    static constexpr uint32_t kDefaultMaxAttempts = 30;
    static constexpr uint64_t kRetryDelayUsec = 1'000'000; // 1s

    SnapshotPhase phase = SnapshotPhase::Idle;
    uint32_t attempts = 0;
    uint32_t maxAttempts = kDefaultMaxAttempts;
    bool serviceOwned = false;
    bool everReady = false;

    SnapshotActions onEvent(SnapshotEvent ev) noexcept
    {
        SnapshotActions a{};

        switch (ev)
        {
            case SnapshotEvent::StartRequested:
                a.cancelRetryTimer = (phase == SnapshotPhase::RetryWait);
                if (phase != SnapshotPhase::InFlight)
                {
                    a.dispatchCall = true;
                }
                break;

            case SnapshotEvent::CallDispatched:
                phase = SnapshotPhase::InFlight;
                ++attempts;
                break;

            case SnapshotEvent::CallSucceeded:
                phase = SnapshotPhase::Ready;
                everReady = true;
                serviceOwned = true;
                attempts = 0;
                a.cancelRetryTimer = true;
                break;

            case SnapshotEvent::CallFailedMissing:
                serviceOwned = false;
                phase = SnapshotPhase::Idle;
                // Wait for ServiceAppeared; do not spin retries.
                a.cancelRetryTimer = true;
                break;

            case SnapshotEvent::CallFailedTransient:
                if (attempts >= maxAttempts)
                {
                    phase = everReady ? SnapshotPhase::Ready
                                      : SnapshotPhase::Idle;
                    a.cancelRetryTimer = true;
                    break;
                }
                // Bounded retry covers "name owned but GMO still failing"
                // (startup races). Missing-name uses CallFailedMissing.
                phase = SnapshotPhase::RetryWait;
                a.armRetryTimer = true;
                a.retryDelayUsec = kRetryDelayUsec;
                break;

            case SnapshotEvent::RetryTimerFired:
                if (phase == SnapshotPhase::RetryWait)
                {
                    a.dispatchCall = true;
                }
                break;

            case SnapshotEvent::ServiceAppeared:
                serviceOwned = true;
                a.cancelRetryTimer = (phase == SnapshotPhase::RetryWait);
                attempts = 0;
                if (phase != SnapshotPhase::InFlight)
                {
                    a.dispatchCall = true;
                }
                break;

            case SnapshotEvent::ServiceDeparted:
                serviceOwned = false;
                a.clearPathMap = true;
                a.cancelRetryTimer = true;
                phase = SnapshotPhase::Idle;
                attempts = 0;
                break;
        }

        return a;
    }
};

/** Classify GetManagedObjects / D-Bus errors for the state machine. */
inline bool isServiceMissingError(std::string_view dbusErrorName) noexcept
{
    return dbusErrorName == "org.freedesktop.DBus.Error.ServiceUnknown" ||
           dbusErrorName == "org.freedesktop.DBus.Error.NameHasNoOwner";
}

} // namespace pwrseq::gpio_props
