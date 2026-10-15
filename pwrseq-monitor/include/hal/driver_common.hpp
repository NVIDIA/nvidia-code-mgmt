/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Shared HAL driver helpers.
 *
 * INPUT lines (PG, BC, boot status): phosphor-multi-gpio-monitor owns the
 * kernel chardev and publishes per-line D-Bus properties
 * (xyz.openbmc_project.Gpio.Monitor.Line). Initial state and live edges
 * arrive as PropertiesChanged → DBusManager::injectLineState →
 * SharedLine::notifyExternal. The daemon never opens or reads input GPIOs.
 *
 * OUTPUT lines (reset): libgpiod-direct via OutputLine (gpio-mon is
 * monitor-only; reset actuation stays in-daemon). ResetChipRegistry caches
 * gpiochip paths process-wide (keyed by line name). On reset the
 * cached chip is tried first; if stale after USB hotplug every entry for
 * that chip path is invalidated, a full rescan runs, and the cache updates.
 */

#pragma once

#include "core/types.hpp"
#include "hal/i_device.hpp"
#include "hal/subscriber_list.hpp"

#include <sdbusplus/bus.hpp>

#include <gpiod.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pwrseq::hal::common
{

/** One GPIO line plus its active-low flag (descriptor polarity). */
struct GpioLine
{
    std::string name;
    bool        activeLow = false;
};

/** Scan /dev/gpiochip* for a chip exposing a line with the given name. */
std::string findChipForLine(const std::string& lineName);

/** Cached logical state for a phosphor-dispatched input line. */
class DbusLineSource
{
  public:
    DbusLineSource(std::string lineName,
                   bool activeLow,
                   bool initialAsserted) noexcept
        : name_(std::move(lineName)),
          activeLow_(activeLow),
          cached_(initialAsserted)
    {}

    int fd() const noexcept { return -1; }

    const std::string& name() const noexcept { return name_; }

    bool read() const noexcept { return cached_; }

    bool hasValue() const noexcept { return hasValue_; }

    bool activeLow() const noexcept { return activeLow_; }

    void setCached(bool asserted) noexcept
    {
        cached_ = asserted;
        hasValue_ = true;
    }

  private:
    std::string name_;
    bool        activeLow_ = false;
    bool        cached_    = false;
    bool        hasValue_  = false;
};

/** RAII handle for a single OUTPUT GPIO line (reset). */
class OutputLine
{
  public:
    OutputLine(const std::string& chip_path,
               const GpioLine& line,
               const std::string& consumer);

    void assert_();
    void release();
    void trigger();

    bool isAsserted() const noexcept { return asserted_; }
    const std::string& name() const noexcept { return name_; }

  private:
    int electricalValueForAssert_() const noexcept
    {
        return active_low_ ? 0 : 1;
    }
    int electricalValueForRelease_() const noexcept
    {
        return active_low_ ? 1 : 0;
    }

    ::gpiod::chip chip_;
    ::gpiod::line line_;
    std::string   name_;
    bool          active_low_ = false;
    bool          asserted_   = false;
};

/** How to drive a reset output during a short-lived acquire. */
enum class ResetLineOp : uint8_t
{
    Trigger,            // assert only (LSTP warm-reset pulse)
    AssertOnly,         // assert and hold until OutputLine is destroyed
    ReleaseElectrical,  // drive release value before dropping the request
};

/** Resolve (scan only, no line request) which gpiochip hosts a line name. */
std::string probeChipForLine(const std::string& lineName);

/** Process-wide cache of reset-line name → gpiochip path. */
class ResetChipRegistry
{
  public:
    /** Return cached chip path, or empty if unknown. */
    std::string get(const std::string& lineName) const;

    /** Probe when uncached; store and return the chip path (may be empty). */
    std::string probeOrStore(const std::string& lineName);

    /** Record a chip path after a successful actuation or rescan. */
    void store(const std::string& lineName, const std::string& chipPath);

    /** Drop one line's entry (e.g. line absent after rescan). */
    void invalidateLine(const std::string& lineName);

    /** Drop every entry pointing at a stale chip path (USB hotplug). */
    void invalidateChip(const std::string& chipPath);

    std::size_t size() const noexcept { return lines_.size(); }

  private:
    std::unordered_map<std::string, std::string> lines_;
};

/** Request, actuate, and release a reset output line.
 *
 *  chips (optional): process-wide cache. When an entry exists the cached
 *  chip is tried first (~1 ms). On failure every entry for that chip path
 *  is invalidated, findChipForLine() runs, and a successful rescan updates
 *  the entry for lineName.
 *
 *  Returns hal::Result::Absent when no gpiochip exposes the line name. */
hal::Result actuateResetOutputLine(const std::string& lineName, bool activeLow,
                                   const std::string& consumer, ResetLineOp op,
                                   ResetChipRegistry* chips = nullptr);

class SharedLine;

class Subscription
{
  public:
    Subscription() noexcept = default;
    Subscription(std::weak_ptr<SharedLine> line, std::uint64_t slot) noexcept
        : line_(std::move(line)), slot_(slot), valid_(true) {}
    ~Subscription() { reset(); }

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& o) noexcept
        : line_(std::move(o.line_)), slot_(o.slot_), valid_(o.valid_)
    {
        o.valid_ = false;
    }

    Subscription& operator=(Subscription&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            line_  = std::move(o.line_);
            slot_  = o.slot_;
            valid_ = o.valid_;
            o.valid_ = false;
        }
        return *this;
    }

    void reset() noexcept;

    bool valid() const noexcept { return valid_; }

  private:
    std::weak_ptr<SharedLine> line_;
    std::uint64_t             slot_  = 0;
    bool                      valid_ = false;
};

/** Refcounted input line fed by phosphor-multi-gpio-monitor edges. */
class SharedLine : public std::enable_shared_from_this<SharedLine>
{
  private:
    struct PrivateTag {};

  public:
    SharedLine(PrivateTag, const GpioLine& gl, bool initialAsserted,
               const std::string& consumer);

    int                fd() const noexcept { return -1; }
    bool               read() const;
    bool               hasValue() const noexcept;
    const std::string& name() const noexcept;
    bool               activeLow() const noexcept;
    const std::string& consumer() const noexcept;

    /** Feed a raw electrical GPIO level; descriptor polarity is applied
     *  before caching and notifying subscribers. */
    void notifyExternal(bool asserted);

    /** Update cached level without notifying subscribers (used when
     *  invalidating stale BC state at reset assert). */
    void setCachedSilently(bool asserted) noexcept
    {
        source_.setCached(asserted);
    }

    Subscription subscribe(LineSubscriber cb);

    std::size_t subscriber_count() const noexcept { return subs_.size(); }

  private:
    friend class SharedLineRegistry;
    friend class Subscription;

    void unsubscribe(std::uint64_t slot) noexcept;

    DbusLineSource source_;
    std::string    consumer_;
    SubscriberList subs_;
};

/** Process-wide registry of phosphor-fed input lines. */
class SharedLineRegistry
{
  public:
    std::shared_ptr<SharedLine> acquire(const GpioLine& gl,
                                        const std::string& consumer);

    std::shared_ptr<SharedLine> findByName(const std::string& lineName) const;

    std::size_t live_count() const;

    template <typename Fn>
    void for_each_alive(Fn&& fn) const
    {
        for (const auto& [_, w] : lines_)
        {
            if (auto sp = w.lock()) fn(*sp);
        }
    }

  private:
    struct Key
    {
        std::string name;
        bool operator==(const Key&) const noexcept = default;
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& k) const noexcept
        {
            return std::hash<std::string>{}(k.name);
        }
    };

    std::unordered_map<Key, std::weak_ptr<SharedLine>, KeyHash> lines_;
};

}  // namespace pwrseq::hal::common
