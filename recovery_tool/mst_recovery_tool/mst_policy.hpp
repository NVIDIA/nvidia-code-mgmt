// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace mst
{

using Value = std::variant<std::string, bool, uint8_t, int16_t, uint16_t,
                           int32_t, uint32_t, int64_t, uint64_t, double,
                           std::vector<std::string>, std::vector<uint64_t>,
                           std::vector<int64_t>, std::vector<double>>;
using PropertyMap = std::map<std::string, Value>;

inline constexpr auto policyInterface =
    "xyz.openbmc_project.Configuration.MSTRecoveryPolicy";

enum class Flow
{
    // Device groups in parallel, FNP asserted for the burn, GIS polled
    Mst,
    // Devices one by one, best-effort GPIOs, no events
    Legacy,
    // One device found through the mapper, FNP held deasserted
    Hold
};

// The platform facts of an MSTRecoveryPolicy record plus the compiled-in
// settings of its DeviceType.
struct Policy
{
    std::string deviceType;
    Flow flow = Flow::Mst;
    // EM board holding the record and its device records
    std::string board;
    std::string deviceInterface;
    std::vector<std::vector<std::string>> groups;
    bool i2cForce = false;
    bool smaPresenceCheck = false;
    bool healthyEvent = false;
    std::vector<uint8_t> gisRegister;
    uint32_t gisHealthyValue = 0;
    std::string gisLabel;
};

// Text the way busctl get-property prints a value after the type prefix
// and quotes are stripped by the scripts.
std::string toText(const Value& value);

// Policy from the record at `path`; rejects keys its flow does not use.
std::expected<Policy, std::string> parsePolicy(const std::string& path,
                                               const PropertyMap& props);

} // namespace mst
