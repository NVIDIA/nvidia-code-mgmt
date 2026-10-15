/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * One-shot read of a single xyz.openbmc_project.Gpio.Monitor.Line object.
 *
 * Live PG/BC edges arrive as PropertiesChanged, so this is only for the
 * reset-timeout sticky-BC check, where the daemon deliberately needs a
 * level that no edge announced. It reads one line object rather than the
 * whole ListGpios table, and takes an explicit short timeout, so a busy
 * gpio-mon cannot stall the daemon's event loop.
 */

#pragma once

#include <sdbusplus/bus.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pwrseq::gpio_mon
{

/** Default read timeout. Short on purpose: the caller is on the event loop
 *  and a missed recovery only costs the pre-existing BOOT_TIMEOUT path. */
constexpr uint64_t kReadTimeoutUsec = 2'000'000;

/** Object path phosphor-gpio-monitor publishes for a kernel line name. */
std::string lineObjectPath(const std::string& lineName);

/** Electrical level of one line, straight from its Line properties.
 *  nullopt when the object is absent, the line is unbound, or the read
 *  fails or times out. */
std::optional<bool> readLineLevel(sdbusplus::bus::bus& bus,
                                  const std::string& lineName,
                                  uint64_t timeoutUsec = kReadTimeoutUsec);

} // namespace pwrseq::gpio_mon
