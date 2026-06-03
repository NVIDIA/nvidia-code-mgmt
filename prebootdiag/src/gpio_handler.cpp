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
#include "gpio_handler.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <system_error>

namespace nvidia::prebootdiag
{

namespace
{
// Releases the line on scope exit so it is returned to the kernel even if
// set_value() throws between request() and release().
struct LineGuard
{
    gpiod::line& line;
    ~LineGuard()
    {
        try
        {
            line.release();
        }
        catch (...)
        {}
    }
};
} // namespace

LibGpioHandler::~LibGpioHandler()
{
    releaseAllPins();
}

bool LibGpioHandler::isPinAvailable(const std::string& lineName)
{
    try
    {
        return static_cast<bool>(gpiod::find_line(lineName));
    }
    catch (const std::exception& e)
    {
        lg2::error("PreBootDiag: GPIO availability check failed {LINE}: {ERR}",
                   "LINE", lineName, "ERR", e.what());
        return false;
    }
}

std::error_code LibGpioHandler::setPin(const std::string& lineName, int value)
{
    try
    {
        auto line = gpiod::find_line(lineName);
        if (!line)
        {
            lg2::error("PreBootDiag: GPIO line not found: {LINE}", "LINE",
                       lineName);
            return std::make_error_code(std::errc::no_such_device);
        }
        line.request({"prebootdiag", gpiod::line_request::DIRECTION_OUTPUT, 0});
        LineGuard guard{line};
        line.set_value(value);

        lg2::info("PreBootDiag: GPIO {LINE}={VAL}", "LINE", lineName, "VAL",
                  value);
        return {};
    }
    catch (const std::exception& e)
    {
        lg2::error("PreBootDiag: GPIO write failed {LINE}={VAL}: {ERR}", "LINE",
                   lineName, "VAL", value, "ERR", e.what());
        return std::make_error_code(std::errc::io_error);
    }
}

std::error_code LibGpioHandler::holdPin(const std::string& lineName, int value)
{
    try
    {
        auto it = heldLines.find(lineName);
        if (it == heldLines.end())
        {
            auto line = gpiod::find_line(lineName);
            if (!line)
            {
                lg2::error("PreBootDiag: GPIO line not found: {LINE}", "LINE",
                           lineName);
                return std::make_error_code(std::errc::no_such_device);
            }
            line.request(
                {"prebootdiag", gpiod::line_request::DIRECTION_OUTPUT, 0},
                value);
            it = heldLines.emplace(lineName, std::move(line)).first;
        }
        else
        {
            it->second.set_value(value);
        }

        lg2::info("PreBootDiag: GPIO {LINE}={VAL} (held)", "LINE", lineName,
                  "VAL", value);
        return {};
    }
    catch (const std::exception& e)
    {
        // Drop any half-requested line so the next holdPin() retries cleanly.
        heldLines.erase(lineName);
        lg2::error("PreBootDiag: GPIO hold failed {LINE}={VAL}: {ERR}", "LINE",
                   lineName, "VAL", value, "ERR", e.what());
        return std::make_error_code(std::errc::io_error);
    }
}

std::error_code
    GpioHandlerInterface::setPins(std::span<const std::string> names, int value)
{
    std::error_code firstErr;
    for (const auto& name : names)
    {
        if (auto ec = setPin(name, value); ec && !firstErr)
        {
            firstErr = ec;
        }
    }
    return firstErr;
}

std::error_code
    GpioHandlerInterface::holdPins(std::span<const std::string> names,
                                   int value)
{
    std::error_code firstErr;
    for (const auto& name : names)
    {
        if (auto ec = holdPin(name, value); ec && !firstErr)
        {
            firstErr = ec;
        }
    }
    return firstErr;
}

void LibGpioHandler::releaseAllPins()
{
    for (auto& [name, line] : heldLines)
    {
        try
        {
            line.release();
        }
        catch (const std::exception& e)
        {
            lg2::warning("PreBootDiag: Failed to release GPIO {LINE}: {ERR}",
                         "LINE", name, "ERR", e.what());
        }
    }
    if (!heldLines.empty())
    {
        lg2::info("PreBootDiag: Released {N} held GPIO lines", "N",
                  heldLines.size());
    }
    heldLines.clear();
}

void LibGpioHandler::releasePins(std::span<const std::string> names)
{
    std::size_t released = 0;
    for (const auto& name : names)
    {
        auto it = heldLines.find(name);
        if (it == heldLines.end())
        {
            continue;
        }
        try
        {
            it->second.release();
        }
        catch (const std::exception& e)
        {
            lg2::warning("PreBootDiag: Failed to release GPIO {LINE}: {ERR}",
                         "LINE", name, "ERR", e.what());
        }
        heldLines.erase(it);
        ++released;
    }
    if (released > 0)
    {
        lg2::info("PreBootDiag: Released {N} held GPIO lines", "N", released);
    }
}

} // namespace nvidia::prebootdiag
