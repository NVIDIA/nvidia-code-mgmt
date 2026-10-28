// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
/*
 * SPDX-License-Identifier: Apache-2.0
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
#include "mcu_recovery_dbus.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <map>
#include <variant>
#include <vector>

namespace nvidia::software::updater::mcu
{

namespace
{

constexpr auto fruService = "xyz.openbmc_project.FruDevice";
constexpr auto fruIface = "xyz.openbmc_project.FruDevice";
constexpr auto fruRoot = "/xyz/openbmc_project/FruDevice";

using FruValue = std::variant<std::string, bool, uint8_t, int16_t, uint16_t,
                              int32_t, uint32_t, int64_t, uint64_t, double>;

std::string numberText(const FruValue& v)
{
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return x;
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return x ? "true" : "false";
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return std::to_string(x);
            }
            else
            {
                return std::to_string(x);
            }
        },
        v);
}

} // namespace

FruLookup dbusFruLookup(sdbusplus::bus_t& bus)
{
    return [&bus, calls = 0](const std::string& fruBus,
                             const std::string& fruAddr) mutable
               -> std::optional<std::string> {
        // The FRU can be late; report D-Bus errors of the first and last try
        ++calls;
        bool report = calls == 1 || calls == fruMaxRetries;
        auto logError = [&](const std::string& path, const std::exception& e) {
            if (report)
            {
                lg2::error("FRU lookup {PATH} failed: {ERROR}", "PATH", path,
                           "ERROR", e.what());
            }
        };
        try
        {
            auto call = bus.new_method_call(
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths");
            call.append(fruRoot, 0, std::vector<std::string>{fruIface});
            std::vector<std::string> paths;
            bus.call(call).read(paths);

            // Paths come back sorted; the first FRU at bus/address wins.
            for (const auto& path : paths)
            {
                std::map<std::string, FruValue> props;
                try
                {
                    auto get = bus.new_method_call(
                        fruService, path.c_str(),
                        "org.freedesktop.DBus.Properties", "GetAll");
                    get.append(fruIface);
                    bus.call(get).read(props);
                }
                catch (const std::exception& e)
                {
                    logError(path, e);
                    continue;
                }
                auto b = props.find("BUS");
                auto a = props.find("ADDRESS");
                if (b == props.end() || a == props.end() ||
                    numberText(b->second) != fruBus ||
                    numberText(a->second) != fruAddr)
                {
                    continue;
                }
                auto p = props.find("BOARD_PART_NUMBER");
                if (p == props.end())
                {
                    return std::string{};
                }
                // The first word, as the shell helper read it.
                auto part = numberText(p->second);
                return part.substr(0, part.find_first_of(" \t"));
            }
        }
        catch (const std::exception& e)
        {
            logError(fruRoot, e);
        }
        return std::nullopt;
    };
}

int JournalBuf::overflow(int c)
{
    if (c == '\n')
    {
        if (error)
        {
            lg2::error("{MSG}", "MSG", line);
        }
        else
        {
            lg2::info("{MSG}", "MSG", line);
        }
        line.clear();
    }
    else if (c != traits_type::eof())
    {
        line += static_cast<char>(c);
    }
    return c;
}

StartUnit dbusStartUnit(sdbusplus::bus_t& bus)
{
    return [&bus](const std::string& unit) {
        auto call = bus.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "StartUnit");
        call.append(unit, "replace");
        // The job is queued, not waited for; a rejected start throws.
        bus.call(call);
    };
}

} // namespace nvidia::software::updater::mcu
