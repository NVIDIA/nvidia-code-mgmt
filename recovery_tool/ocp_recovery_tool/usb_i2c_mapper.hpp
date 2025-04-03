/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <sdbusplus/server.hpp>

#include <filesystem>
#include <string>

namespace recovery_tool
{

namespace usb_i2c
{

constexpr auto usbDevicePath = "/sys/bus/usb/devices/";
constexpr auto i2cBusPrefix = "i2c-";
constexpr size_t i2cBusPrefixLength = std::string_view(i2cBusPrefix).length();

/**
 * @brief Get I2C bus number from USB port path
 *
 * @param[in] usbPort The USB port to check (e.g., "1-1.2.2:1.1")
 * @param[in] verbose Whether to print verbose output
 *
 * @return The I2C bus number if found, -1 otherwise
 */
int getI2CBusFromUSBPort(const std::string& usbPort, bool verbose = false);

} // namespace usb_i2c
} // namespace recovery_tool