// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "mst_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>

namespace mst
{

namespace
{

struct TypeSettings
{
    std::string_view type;
    std::array<uint8_t, 4> gisRegister;
    uint32_t gisHealthyValue;
    std::string_view gisLabel;
    // A record without the flow keys means the Mst flow with the SMA
    // presence check and FirmwareNotInRecovery events
    bool mstDefaults;
};

constexpr std::array<TypeSettings, 3> typeSettings{{
    {"ConnectX",
     {0x00, 0x05, 0x50, 0x84},
     0x20000019,
     "irisc.global_image_status",
     false},
    {"NVLinkManagementNIC",
     {0x00, 0x05, 0x50, 0x84},
     0x20000019,
     "irisc.global_image_status",
     true},
    {"NVSwitch", {0x00, 0x15, 0x20, 0x80}, 0x5e, "global_image_status", true},
}};

constexpr std::array<std::pair<std::string_view, Flow>, 3> flows{{
    {"Mst", Flow::Mst},
    {"Legacy", Flow::Legacy},
    {"Hold", Flow::Hold},
}};

template <class T>
const T* get(const PropertyMap& props, const std::string& key)
{
    auto it = props.find(key);
    return it == props.end() ? nullptr : std::get_if<T>(&it->second);
}

} // namespace

std::string toText(const Value& value)
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
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", x);
                return buf;
            }
            else if constexpr (std::is_integral_v<T>)
            {
                return std::to_string(x);
            }
            else
            {
                std::string out;
                for (const auto& v : x)
                {
                    out += (out.empty() ? "" : " ") + toText(Value{v});
                }
                return out;
            }
        },
        value);
}

std::expected<Policy, std::string> parsePolicy(const std::string& path,
                                               const PropertyMap& props)
{
    const auto* type = get<std::string>(props, "DeviceType");
    const auto* ts = type == nullptr ? typeSettings.end()
                                     : std::ranges::find(typeSettings, *type,
                                                         &TypeSettings::type);
    if (ts == typeSettings.end())
    {
        return std::unexpected("unsupported DeviceType");
    }
    Policy p;
    p.deviceType = *type;
    p.board = path.substr(0, path.find_last_of('/'));
    p.gisRegister.assign(ts->gisRegister.begin(), ts->gisRegister.end());
    p.gisHealthyValue = ts->gisHealthyValue;
    p.gisLabel = ts->gisLabel;

    if (const auto* name = get<std::string>(props, "RecoveryFlow"))
    {
        const auto* f = std::ranges::find(
            flows, *name, &std::pair<std::string_view, Flow>::first);
        if (f == flows.end())
        {
            return std::unexpected("unsupported RecoveryFlow " + *name);
        }
        p.flow = f->second;
    }
    else if (!ts->mstDefaults)
    {
        return std::unexpected("RecoveryFlow is missing");
    }

    std::vector<std::string_view> keys{"Name", "Type", "DeviceType",
                                       "RecoveryFlow"};
    if (p.flow != Flow::Hold)
    {
        keys.emplace_back("RecoveryGroups");
    }
    if (p.flow == Flow::Mst)
    {
        keys.insert(keys.end(),
                    {"I2CForce", "SMAPresenceCheck", "HealthyEvent"});
    }
    for (const auto& key : props | std::views::keys)
    {
        if (!std::ranges::contains(keys, key))
        {
            return std::unexpected(key + " does not apply to this flow");
        }
    }

    if (const auto* groups =
            get<std::vector<std::string>>(props, "RecoveryGroups"))
    {
        for (const auto& group : *groups)
        {
            std::istringstream in(group);
            std::vector<std::string> members;
            for (std::string m; in >> m;)
            {
                members.push_back(m);
            }
            if (!members.empty())
            {
                p.groups.push_back(std::move(members));
            }
        }
    }
    if (p.flow != Flow::Hold && p.groups.empty())
    {
        return std::unexpected("RecoveryGroups is missing or empty");
    }
    if (p.flow == Flow::Mst)
    {
        for (auto [key, field, fallback] :
             {std::tuple{"I2CForce", &p.i2cForce, false},
              std::tuple{"SMAPresenceCheck", &p.smaPresenceCheck, true},
              std::tuple{"HealthyEvent", &p.healthyEvent, true}})
        {
            const auto* v = get<bool>(props, key);
            if (v == nullptr && (props.contains(key) || !ts->mstDefaults))
            {
                return std::unexpected(std::string(key) +
                                       " is missing or not a boolean");
            }
            *field = v != nullptr ? *v : fallback;
        }
    }
    p.deviceInterface =
        "xyz.openbmc_project.Configuration." +
        (p.flow == Flow::Legacy ? std::string("CX8") : p.deviceType) +
        "Recovery";
    return p;
}

} // namespace mst
