/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once
// Fake gpiod.hpp for unit tests — intercepts libgpiod C++ calls so that
// force_recovery.cpp can be included directly without real GPIO hardware.

#include <map>
#include <stdexcept>
#include <string>

namespace gpiod
{

// Per-pin behaviour controlled by each test case
struct FakeGpioState
{
    bool findSucceeds = true;
    bool requestThrows = false;
    bool setValueThrows = false;
    std::string throwMsg = "mock gpio error";
    int lastSetValue = -1;
};

inline std::map<std::string, FakeGpioState>& gpioFakes()
{
    static std::map<std::string, FakeGpioState> s;
    return s;
}

struct line_request
{
    std::string consumer;
    int request_type = 0;
    int flags = 0;
    static constexpr int DIRECTION_OUTPUT = 0;
};

class line
{
  public:
    line() = default;
    line(const line&) = default;
    line& operator=(const line&) = default;
    line(line&&) = default;
    line& operator=(line&&) = default;

    explicit operator bool() const
    {
        return valid_;
    }

    void request(const line_request&, int /*defaultVal*/)
    {
        auto it = gpioFakes().find(name_);
        if (it != gpioFakes().end() && it->second.requestThrows)
            throw std::runtime_error(it->second.throwMsg);
    }

    void set_value(int val)
    {
        auto& state = gpioFakes()[name_];
        if (state.setValueThrows)
            throw std::runtime_error(state.throwMsg);
        state.lastSetValue = val;
    }

    // called only by find_line()
    void setName(const std::string& n)
    {
        name_ = n;
    }
    void setValid(bool v)
    {
        valid_ = v;
    }

  private:
    std::string name_;
    bool valid_ = false;
};

inline line find_line(const std::string& name)
{
    line l;
    l.setName(name);
    auto it = gpioFakes().find(name);
    l.setValid(it == gpioFakes().end() || it->second.findSucceeds);
    return l;
}

} // namespace gpiod
