// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

/**
 * Fake <gpiod.hpp> for USB DFU recovery unit tests.
 *
 * Placed in test/usbdfu_fakes/ so that it shadows the real <gpiod.hpp> when
 * this directory is first in the compiler include path.  Test state is kept in
 * test::usbdfu_fake_gpio::lines — a plain map from GPIO name to LineState.
 * Tests populate the map in SetUp() and inspect it after calling production
 * code to verify which lines were requested, set, and released.
 */

#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test::usbdfu_fake_gpio
{

struct LineState
{
    // Controls find_line() — set to false to simulate a missing GPIO line
    bool present = true;
    // If true, line::request() throws std::runtime_error
    bool throwOnRequest = false;
    // If true, line::release() throws std::runtime_error (currently unused by
    // production code, but available for completeness)
    bool throwOnRelease = false;
    // If true, line::set_value() throws std::runtime_error on the Nth call
    // (throwOnSetAfter == -1 means always throw; otherwise throw after N calls)
    bool throwOnSet = false;
    int throwOnSetAfter = -1; // -1 = always; 0 = first call; N = after N calls

    bool requested = false;
    bool released = false;
    std::size_t requestCount = 0;
    std::size_t releaseCount = 0;
    int requestDefault = -1;

    // All values passed to set_value(), in order
    std::vector<int> setValues;
};

inline std::map<std::string, LineState> lines{};

// If true, gpiod::find_line() throws std::runtime_error (simulates a chip
// that cannot be opened).
inline bool throwOnFindLine = false;

inline void reset()
{
    lines.clear();
    throwOnFindLine = false;
}

} // namespace test::usbdfu_fake_gpio

// ---------------------------------------------------------------------------
// Fake gpiod C++ API — mirrors libgpiod 1.x for the subset used by
// perform_dfu_recovery.cpp.
// ---------------------------------------------------------------------------

namespace gpiod
{

struct line_request
{
    // Subset of direction constants used by perform_dfu_recovery.cpp
    enum : int
    {
        DIRECTION_INPUT = 0,
        DIRECTION_OUTPUT = 1,
    };

    std::string consumer;
    int request_type{DIRECTION_OUTPUT};
    int flags{0};
};

class line
{
  public:
    line() = default;

    line(std::string lineName, bool validLine) :
        name_(std::move(lineName)), valid_(validLine)
    {}

    explicit operator bool() const noexcept
    {
        return valid_;
    }

    /**
     * Request this line for output.  Matches the real gpiod 1.x signature:
     *   void request(const line_request& config, int default_val = 0) const;
     *
     * Production code calls it with one argument:
     *   line.request({"usbdfu-recovery",
     *                  gpiod::line_request::DIRECTION_OUTPUT, 0});
     */
    void request(const line_request& /*cfg*/, int defaultValue = 0) const
    {
        auto& state = stateFor();
        if (state.throwOnRequest)
        {
            throw std::runtime_error("fake gpio request failure");
        }
        state.requested = true;
        state.requestDefault = defaultValue;
        ++state.requestCount;
    }

    void release() const
    {
        if (!valid_)
            return; // No-op for invalid (already-released) lines
        auto& state = stateFor();
        if (state.throwOnRelease)
        {
            throw std::runtime_error("fake gpio release failure");
        }
        state.released = true;
        ++state.releaseCount;
        // Mirror the real libgpiod behavior: the line becomes invalid after
        // release.  Using mutable so this can be called on const instances.
        valid_ = false;
    }

    void set_value(int value) const
    {
        auto& state = stateFor();
        if (state.throwOnSet)
        {
            // Check the "after N calls" threshold
            if (state.throwOnSetAfter < 0 ||
                static_cast<int>(state.setValues.size()) >=
                    state.throwOnSetAfter)
            {
                throw std::runtime_error("fake gpio set failure");
            }
        }
        state.setValues.push_back(value);
    }

  private:
    test::usbdfu_fake_gpio::LineState& stateFor() const
    {
        if (!valid_)
        {
            throw std::runtime_error("operation on invalid fake gpio line: " +
                                     name_);
        }
        return test::usbdfu_fake_gpio::lines[name_];
    }

    std::string name_{};
    mutable bool valid_{false};
};

inline line find_line(const std::string& name)
{
    if (test::usbdfu_fake_gpio::throwOnFindLine)
    {
        throw std::runtime_error("fake find_line failure");
    }
    auto it = test::usbdfu_fake_gpio::lines.find(name);
    if (it == test::usbdfu_fake_gpio::lines.end() || !it->second.present)
    {
        return line(name, false);
    }
    return line(name, true);
}

} // namespace gpiod
