/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <map>
#include <string>
#include <string_view>
#include <variant>

// Thin synchronous client for the IGX platform-state daemon
// (com.Nvidia.Platform). Replaces the popen() calls into the legacy
// igx-platform-detection.py / smcu-version.py helpers: the daemon is now the
// single source of truth for platform identity + SMCU firmware state.
namespace nvidia::platform
{

inline constexpr auto service = "com.Nvidia.Platform";
inline constexpr auto objPath = "/com/Nvidia/Platform";
inline constexpr auto infoIface = "com.Nvidia.Platform.Info";
inline constexpr auto smcuIface = "com.Nvidia.Platform.Smcu";

// Read a string property; returns "" on any error (daemon down, wrong type).
// sdbusplus bus::call() takes the timeout in MICROSECONDS.
inline std::string getString(sdbusplus::bus_t& bus, const char* iface,
                             const char* prop)
{
    try
    {
        auto m = bus.new_method_call(service, objPath,
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append(iface, prop);
        auto reply = bus.call(m, 5000000);
        std::variant<std::string> v;
        reply.read(v);
        return std::get<std::string>(v);
    }
    catch (const std::exception& e)
    {
        lg2::warning("platform_client: Get {IF}.{P} failed: {E}", "IF", iface,
                     "P", prop, "E", e.what());
        return "";
    }
}

using PlatformProperty = std::variant<std::string, bool>;
using PlatformProperties = std::map<std::string, PlatformProperty>;

inline std::string
    platformShortFromProperties(const PlatformProperties& properties)
{
    const auto value = [&properties](std::string_view name) -> std::string {
        const auto it = properties.find(std::string{name});
        if (it == properties.end())
        {
            return {};
        }
        const auto* result = std::get_if<std::string>(&it->second);
        return result == nullptr ? std::string{} : *result;
    };

    const auto ready = properties.find("IdentityReady");
    if (ready == properties.end())
    {
        return {};
    }
    const auto* identityReady = std::get_if<bool>(&ready->second);
    if (identityReady == nullptr || !*identityReady)
    {
        return {};
    }

    const std::string name = value("Name");
    if (name.find("Thor") != std::string::npos)
    {
        return "Thor";
    }
    if (name.find("Orin") != std::string::npos)
    {
        return "Orin";
    }
    return "Host";
}

// "Thor" / "Orin" / "Host" / "" (empty = unknown/unready). IdentityReady
// remains valid while unrelated SMCU health is Degraded, avoiding inventory
// churn during an SMCU flash. Empty is distinct from confirmed Host.
inline std::string platformShort(sdbusplus::bus_t& bus)
{
    try
    {
        auto m = bus.new_method_call(
            service, objPath, "org.freedesktop.DBus.Properties", "GetAll");
        m.append(infoIface);
        auto reply = bus.call(m, 5000000);
        PlatformProperties properties;
        reply.read(properties);
        return platformShortFromProperties(properties);
    }
    catch (const std::exception& e)
    {
        lg2::warning("platform_client: GetAll {IF} failed: {E}", "IF",
                     infoIface, "E", e.what());
        return {};
    }
}

// "ASMCU" / "RSMCU" / "" (empty when unknown). Mirrors the legacy
// igx-platform-detection.py --type smcu contract.
inline std::string smcuModel(sdbusplus::bus_t& bus)
{
    const std::string shortName = platformShort(bus);
    if (shortName.empty() || shortName == "Host")
    {
        return {};
    }
    const std::string model = getString(bus, smcuIface, "Model");
    return (model == "NA") ? std::string{} : model;
}

inline std::string formatSmcuVersion(const std::string& fw,
                                     const std::string& ver)
{
    const bool fwOk = !fw.empty() && fw != "NA";
    const bool verOk = !ver.empty() && ver != "NA";
    if (fwOk && verOk)
    {
        return fw + " " + ver;
    }
    if (verOk)
    {
        return ver;
    }
    if (fwOk)
    {
        return fw;
    }
    return "--";
}

// "FW VER" (e.g. "NFW 2.00.00") or the best available component.
inline std::string smcuVersionString(sdbusplus::bus_t& bus)
{
    return formatSmcuVersion(getString(bus, smcuIface, "FwType"),
                             getString(bus, smcuIface, "Version"));
}

// One-shot variants that open their own system bus, for const/no-bus callers.
inline std::string smcuModelOneShot()
{
    auto bus = sdbusplus::bus::new_default_system();
    return smcuModel(bus);
}

inline std::string smcuVersionOneShot()
{
    auto bus = sdbusplus::bus::new_default_system();
    return smcuVersionString(bus);
}

} // namespace nvidia::platform
