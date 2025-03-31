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

#include "utils.hpp"

namespace mcu_recovery_manager
{

std::map<std::string, MCUInfo> parseJsonFile(const std::string& jsonFilePath)
{
    std::ifstream file(jsonFilePath);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open JSON file: " + jsonFilePath);
    }

    json j;
    file >> j;

    if (file.fail())
    {
        throw std::runtime_error("Failed to parse JSON file: " + jsonFilePath);
    }

    std::map<std::string, MCUInfo> mcuMap;
    for (const auto& item : j["Exposes"])
    {
        // Check if Type is MCURecovery and skip if not
        if (item["Type"] != "MCURecovery")
        {
            continue;
        }

        MCUInfo info;
        info.usbPort = item["USBPort"];
        info.resetGpioName = item["ResetGpioName"];
        info.recoveryGpioName = item["RecoveryGpioName"];
        info.functionalPid =
            std::stoi(item["ProductId"].get<std::string>(), nullptr, 16);
        info.device = item["Name"];
        mcuMap[info.usbPort] = info;
    }
    return mcuMap;
}

std::string toHexString(uint16_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
        << value;
    return oss.str();
}

std::string executeCommand(const std::string& cmd)
{
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, void (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),
                                                [](FILE* f) { pclose(f); });

    if (!pipe)
    {
        throw std::runtime_error("popen() failed");
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        result += buffer.data();
    }

    return result;
}

bool isCommandSuccessful(const std::string& output)
{
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("Response status") != std::string::npos &&
            line.find("Success") != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace mcu_recovery_manager
