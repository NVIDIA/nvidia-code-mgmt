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
#pragma once

#include <chrono>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <string>

namespace nvidia::software::updater::mcu
{

// /etc/mcurecovery/mcurecovery.env, one KEY=VALUE per line:
//   INSTANCE_COUNT, TARGET_NAME_<n>            instances the starter runs
//   <inst>_MODEL                                fixed model of an instance
//   <inst>_VARIANT_DETECT=FRU                   model from the FRU instead:
//   <inst>_FRU_BUS, <inst>_FRU_ADDR             the FRU to read
//   <inst>_VARIANT_COUNT                        number of variants
//   <inst>_VARIANT_<n>_KEY, _MODEL              first KEY contained in the
//                                               FRU BOARD_PART_NUMBER wins
inline constexpr auto envFile = "/etc/mcurecovery/mcurecovery.env";

using Env = std::map<std::string, std::string>;

Env parseEnv(std::istream& in);

// BOARD_PART_NUMBER of the FruDevice at bus/address (decimal, as busctl
// prints them), nullopt when no FRU there yet.
using FruLookup = std::function<std::optional<std::string>(
    const std::string& bus, const std::string& address)>;
using Sleep = std::function<void(std::chrono::seconds)>;

inline constexpr int fruMaxRetries = 60;
inline constexpr std::chrono::seconds fruRetryInterval{5};

// The model code-manager -u MCURecovery runs with, or nullopt after
// reporting the reason on err.
std::optional<std::string> resolveModel(const Env& env,
                                        const std::string& instance,
                                        const FruLookup& lookup,
                                        const Sleep& sleep, std::ostream& err);

// Starts com.Nvidia.MCURecovery.Updater@<TARGET_NAME_n>.service for every
// instance. Returns the process exit code.
using StartUnit = std::function<void(const std::string& unit)>;
int startUpdaters(const Env& env, const StartUnit& start, std::ostream& out);

} // namespace nvidia::software::updater::mcu
