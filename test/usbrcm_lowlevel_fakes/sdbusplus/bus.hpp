/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once
// Fake sdbusplus/bus.hpp for test_usbrcm_lowlevel — lets dbusReset() succeed
// or fail on demand without a real D-Bus connection.

#include <stdexcept>
#include <string>

namespace test::usbrcm_fake_bus
{

inline bool callThrows = false;
inline std::string throwMessage{"D-Bus call failed (fake)"};

inline void setCallThrows(bool throws,
                          std::string msg = "D-Bus call failed (fake)")
{
    callThrows = throws;
    throwMessage = std::move(msg);
}

inline void reset()
{
    callThrows = false;
    throwMessage = "D-Bus call failed (fake)";
}

} // namespace test::usbrcm_fake_bus

namespace sdbusplus::bus
{

class message
{
  public:
    template <typename... Args>
    void append(Args&&...)
    {}
};

class bus
{
  public:
    template <typename... Args>
    message new_method_call(Args&&...)
    {
        return {};
    }

    void call(const message&)
    {
        if (test::usbrcm_fake_bus::callThrows)
            throw std::runtime_error(test::usbrcm_fake_bus::throwMessage);
    }
};

inline bus new_default()
{
    return {};
}

} // namespace sdbusplus::bus

namespace sdbusplus
{
using bus_t = bus::bus;
} // namespace sdbusplus
