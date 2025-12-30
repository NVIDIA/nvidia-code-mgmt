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

#include "mcu_recovery_manager.hpp"

#include <libusb-1.0/libusb.h>

#include <gpiod.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace mcu_recovery_manager
{
std::map<std::string, MCUInfo> parseJsonFile(const std::string& jsonFilePath);
std::map<std::string, MCUInfo>
    getMCUConfigByTargetFromDbus(const std::string& target);
std::map<std::string, MCUInfo> getAllMCUConfigFromDbus();
std::map<std::string, MCUInfo>
    getMCUConfigByChassisFromDbus(const std::string& chassisName);
std::string toHexString(uint16_t value);
std::string executeCommand(const std::string& cmd);
bool isCommandSuccessful(const std::string& output);
} // namespace mcu_recovery_manager