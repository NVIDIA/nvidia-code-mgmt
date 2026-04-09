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

#include "smcu_updater.hpp"

#include <phosphor-logging/lg2.hpp>

#include <array>
#include <cstdio>
#include <memory>

namespace nvidia
{
namespace software
{
namespace updater
{

// Helper function to execute command and read output
static std::string executeCommand(const std::string& cmd)
{
    std::string output;
    std::array<char, 128> buffer;

    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
    {
        lg2::error("Failed to execute command: {CMD}", "CMD", cmd);
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        output += buffer.data();
    }

    // Remove trailing newline/whitespace
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                               output.back() == ' '))
    {
        output.pop_back();
    }

    return output;
}

std::string SMCUItemUpdater::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    // Get device type from inventory path
    std::string deviceType;

    if (inventoryPath.ends_with("/ASMCU"))
    {
        deviceType = "ASMCU";
    }
    else if (inventoryPath.ends_with("/RSMCU"))
    {
        deviceType = "RSMCU";
    }
    else
    {
        // Unknown device type
        return "";
    }

    // Get actual firmware version from smcu-version.py
    try
    {
        std::string cmd = "/usr/bin/smcu-version.py";
        std::string fwVersion = executeCommand(cmd);

        if (fwVersion.empty() || fwVersion == "--")
        {
            // Fallback to device type only if version not available
            lg2::warning(
                "SMCU firmware version not available, using device type only");
            return deviceType;
        }

        // Combine device type and firmware version: "ASMCU-NFW 2.00.00"
        std::string combinedVersion = deviceType + "-" + fwVersion;
        lg2::info("SMCU version: {VERSION}", "VERSION", combinedVersion);
        return combinedVersion;
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to get SMCU firmware version: {ERROR}", "ERROR",
                   e.what());
        // Fallback to device type only
        return deviceType;
    }
}

std::string SMCUItemUpdater::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "NVIDIA";
}

std::string SMCUItemUpdater::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    if (inventoryPath.ends_with("/ASMCU"))
    {
        return "ASMCU";
    }
    if (inventoryPath.ends_with("/RSMCU"))
    {
        return "RSMCU";
    }
    return "NVIDIA";
}

} // namespace updater
} // namespace software
} // namespace nvidia
