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
#include "mcu_recovery_model.hpp"

#include <charconv>
#include <exception>
#include <string_view>

namespace nvidia::software::updater::mcu
{

namespace
{

std::string_view trim(std::string_view s)
{
    auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos)
    {
        return {};
    }
    auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string get(const Env& env, const std::string& key)
{
    auto it = env.find(key);
    return it == env.end() ? std::string{} : it->second;
}

std::optional<int> toInt(const std::string& s)
{
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
    {
        return std::nullopt;
    }
    return v;
}

} // namespace

Env parseEnv(std::istream& in)
{
    Env env;
    std::string line;
    while (std::getline(in, line))
    {
        auto l = trim(line);
        if (l.empty() || l.front() == '#')
        {
            continue;
        }
        auto eq = l.find('=');
        if (eq == std::string_view::npos)
        {
            continue;
        }
        auto key = trim(l.substr(0, eq));
        auto val = trim(l.substr(eq + 1));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
            val.back() == val.front())
        {
            val = val.substr(1, val.size() - 2);
        }
        env[std::string(key)] = std::string(val);
    }
    return env;
}

std::optional<std::string> resolveModel(const Env& env,
                                        const std::string& instance,
                                        const FruLookup& lookup,
                                        const Sleep& sleep, std::ostream& err)
{
    if (instance.empty())
    {
        err << "Error: instance name required\n";
        return std::nullopt;
    }

    if (get(env, instance + "_VARIANT_DETECT") != "FRU")
    {
        auto model = get(env, instance + "_MODEL");
        if (model.empty())
        {
            err << "Error: " << instance
                << "_MODEL not set in environment file\n";
            return std::nullopt;
        }
        return model;
    }

    auto bus = get(env, instance + "_FRU_BUS");
    auto addr = get(env, instance + "_FRU_ADDR");
    if (bus.empty() || addr.empty())
    {
        err << "Error: " << instance << "_FRU_BUS or " << instance
            << "_FRU_ADDR not set\n";
        return std::nullopt;
    }

    std::string boardPart;
    for (int retry = 0; retry < fruMaxRetries;)
    {
        boardPart = lookup(bus, addr).value_or("");
        if (!boardPart.empty())
        {
            break;
        }
        ++retry;
        err << "FRU not found at BUS=" << bus << " ADDR=" << addr
            << ", retrying in " << fruRetryInterval.count() << "s (" << retry
            << "/" << fruMaxRetries << ")...\n";
        sleep(fruRetryInterval);
    }
    if (boardPart.empty())
    {
        err << "Error: No FRU found at BUS=" << bus << " ADDR=" << addr
            << " after " << fruMaxRetries * fruRetryInterval.count() << "s\n";
        return std::nullopt;
    }

    auto countStr = get(env, instance + "_VARIANT_COUNT");
    if (countStr.empty())
    {
        err << "Error: " << instance << "_VARIANT_COUNT not set\n";
        return std::nullopt;
    }
    auto count = toInt(countStr);
    if (!count || *count < 0)
    {
        err << "Error: " << instance << "_VARIANT_COUNT is not a number\n";
        return std::nullopt;
    }
    for (int i = 0; i < *count; ++i)
    {
        auto prefix = instance + "_VARIANT_" + std::to_string(i);
        auto key = get(env, prefix + "_KEY");
        auto model = get(env, prefix + "_MODEL");
        if (key.empty() || model.empty())
        {
            err << "Error: " << prefix << "_KEY or " << prefix
                << "_MODEL not set\n";
            return std::nullopt;
        }
        if (boardPart.find(key) != std::string::npos)
        {
            return model;
        }
    }

    err << "Error: Unknown " << instance
        << " variant, BOARD_PART_NUMBER=" << boardPart << "\n";
    return std::nullopt;
}

int startUpdaters(const Env& env, const StartUnit& start, std::ostream& out)
{
    auto countStr = get(env, "INSTANCE_COUNT");
    if (countStr.empty())
    {
        out << "Error: INSTANCE_COUNT not set\n";
        return 1;
    }
    auto count = toInt(countStr);
    if (!count)
    {
        out << "Error: INSTANCE_COUNT is not a number\n";
        return 1;
    }
    if (*count < 0)
    {
        out << "Error: INSTANCE_COUNT is negative\n";
        return 1;
    }
    for (int i = 0; i < *count; ++i)
    {
        auto target = get(env, "TARGET_NAME_" + std::to_string(i));
        if (target.empty())
        {
            out << "Error: TARGET_NAME_" << i << " not set\n";
            return 1;
        }
        auto unit = "com.Nvidia.MCURecovery.Updater@" + target + ".service";
        out << "Starting " << unit << "\n";
        try
        {
            start(unit);
        }
        catch (const std::exception& e)
        {
            out << "Error: starting " << unit << ": " << e.what() << "\n";
            return 1;
        }
    }
    return 0;
}

} // namespace nvidia::software::updater::mcu
