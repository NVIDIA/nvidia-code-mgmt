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

#include "config.h"

#include "nvme_updater.hpp"

#include <unistd.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{

namespace
{
std::string quoteEnvValue(const std::string& value)
{
    std::string quoted = "\"";
    for (const char c : value)
    {
        // Keep each EnvironmentFile assignment on a single physical line.
        if (c == '\n')
        {
            quoted += "\\n";
            continue;
        }
        if (c == '\r')
        {
            quoted += "\\r";
            continue;
        }
        if (c == '\0')
        {
            quoted += "\\0";
            continue;
        }
        if (c == '\\' || c == '"' || c == '$' || c == '`')
        {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

std::string makeNvmeUpdateRequestId()
{
    static std::atomic_uint64_t counter{0};
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return "nvme-" + std::to_string(getpid()) + "-" + std::to_string(now) +
           "-" + std::to_string(counter.fetch_add(1));
}

std::filesystem::path nvmeRequestFilePath(const std::string& requestId)
{
    return std::filesystem::path(NVME_UPDATE_REQUEST_DIR) /
           (requestId + ".env");
}

} // namespace

std::string NVMeItemUpdater::getVersion(const std::string& inventoryPath) const
{
    std::string ret = "";

    try
    {
        ret = getProperty<std::string>(NVME_BUSNAME_INVENTORY,
                                       inventoryPath.c_str(), VERSION_IFACE,
                                       VERSION);
    }
    catch (const std::exception& e)
    {
        // ignore the exception for NVMe
    }
    return ret;
}

std::string NVMeItemUpdater::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    std::string ret = "";

    try
    {
        ret = getProperty<std::string>(NVME_BUSNAME_INVENTORY,
                                       inventoryPath.c_str(), ASSET_IFACE,
                                       MANUFACTURER);
    }
    catch (const std::exception& e)
    {
        // ignore the exception for NVMe
    }
    return ret;
}

std::string NVMeItemUpdater::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    std::string ret = "";
    try
    {
        ret = getProperty<std::string>(NVME_BUSNAME_INVENTORY,
                                       inventoryPath.c_str(), ASSET_IFACE,
                                       PART_NUMBER);

        // Get the second word from the part number (space-separated)
        // e.g. "SAMSUNG MZTL23T8HCLS-00A07" -> "MZTL23T8HCLS-00A07"
        if (!ret.empty())
        {
            std::istringstream iss(ret);
            std::vector<std::string> words;
            std::string word;

            // Split the string by spaces
            while (iss >> word)
            {
                words.push_back(word);
            }

            // Get the second word if it exists
            if (words.size() >= 2)
            {
                ret = words[1];
            }
            else if (words.size() == 1)
            {
                ret = words[0];
            }
            else
            {
                ret = "";
            }
        }
    }
    catch (const std::exception& e)
    {
        // ignore the exception for NVMe
    }
    return ret;
}

/**
 * @brief Extract model name from composite format
 *
 * Handles both simple model format and composite format.
 * Composite format: "Manufacturer:Model:UUID"
 * Simple format: "Model"
 *
 * @param compositeModel The model string (may be composite or simple)
 * @return std::string The extracted model name
 */
static std::string extractModelFromComposite(const std::string& compositeModel)
{
    if (compositeModel.empty())
    {
        return "";
    }

    // Check if it's in composite format (contains colons)
    size_t firstColon = compositeModel.find(':');
    if (firstColon == std::string::npos)
    {
        // Simple format, return as-is
        return compositeModel;
    }

    // Composite format: extract middle part between first and second colon
    size_t secondColon = compositeModel.find(':', firstColon + 1);
    if (secondColon == std::string::npos)
    {
        // Only one colon, return everything after it
        return compositeModel.substr(firstColon + 1);
    }

    // Extract "Model" from "Manufacturer:Model:UUID"
    return compositeModel.substr(firstColon + 1, secondColon - firstColon - 1);
}

/**
 * @brief Check if device model matches the filter model
 *
 * Compares the device model (from D-Bus) with the filter model.
 * The filter model may be in composite format "Manufacturer:Model:UUID"
 * and will be automatically extracted for comparison.
 *
 * @param deviceModel The model from the device (from getModel())
 * @param filterModel The filter model (may be composite or simple)
 * @return bool True if models match or no filter, false otherwise
 */
static bool isModelMatch(const std::string& deviceModel,
                         const std::string& filterModel)
{
    // No filter means match everything
    if (filterModel.empty())
    {
        return true;
    }

    // Extract model from composite format if needed
    std::string modelToMatch = extractModelFromComposite(filterModel);

    // Compare
    return deviceModel == modelToMatch;
}

static std::string driveNameFromPath(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string NVMeItemUpdater::validateTarget(
    const sdbusplus::message::object_path& target)
{
    std::string driveName = target.filename();
    if (driveName.starts_with("FW_"))
    {
        driveName.erase(0, 3);
    }
    return driveName;
}

/**
 * @brief Get NVMe drives to update based on target filters
 *
 * @param targetFilter
 * @return std::string representing drive names to update
 */
std::string
    NVMeItemUpdater::getDevicesToUpdate(const TargetFilter& targetFilter) const
{
    std::string driveList;
    if (targetFilter.type == TargetFilterType::UpdateSelected)
    {
        // Get all drive objects from inventory to check model names
        try
        {
            ObjectValueTree objects =
                getManagedObjects(NVME_BUSNAME_INVENTORY,
                                  "/xyz/openbmc_project/inventory/system/nvme");

            for (auto& target : targetFilter.targets)
            {
                if (target.empty())
                {
                    continue;
                }

                // Find the corresponding drive object for this drive name
                for (auto& [path, interfaces] : objects)
                {
                    // Check if this object implements the Drive interface
                    if (interfaces.find(NVME_INVENTORY_IFACE) ==
                        interfaces.end())
                    {
                        continue;
                    }

                    std::string pathStr = path.str;
                    std::string driveName = driveNameFromPath(pathStr);
                    if (driveName == target)
                    {
                        // Get the model property
                        try
                        {
                            std::string driveModel = getModel(pathStr);

                            // Check if model matches
                            if (isModelMatch(driveModel, modelName))
                            {
                                driveList += driveName + " ";
                            }
                        }
                        catch (const std::exception& e)
                        {
                            lg2::error(
                                "Failed to get model for drive {DRIVE}: {ERROR}",
                                "DRIVE", driveName, "ERROR", e.what());
                        }
                        break;
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to get NVMe device paths for UpdateSelected: {ERROR}",
                "ERROR", e.what());
        }
    }
    else if (targetFilter.type == TargetFilterType::UpdateAll)
    {
        try
        {
            ObjectValueTree objects =
                getManagedObjects(NVME_BUSNAME_INVENTORY,
                                  "/xyz/openbmc_project/inventory/system/nvme");

            for (auto& [path, interfaces] : objects)
            {
                std::string pathStr = path.str;

                // Check if this object implements the Drive interface
                if (interfaces.find(NVME_INVENTORY_IFACE) == interfaces.end())
                {
                    continue;
                }

                std::string driveName = driveNameFromPath(pathStr);
                if (driveName.empty())
                {
                    continue;
                }

                // Get the model property and check if it matches
                try
                {
                    std::string driveModel = getModel(pathStr);

                    // Check if model matches
                    if (isModelMatch(driveModel, modelName))
                    {
                        driveList += driveName + " ";
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "UpdateAll: Failed to get model for drive {DRIVE} at {PATH}: {ERROR}",
                        "DRIVE", driveName, "PATH", pathStr, "ERROR", e.what());
                }
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("UpdateAll: Failed to get NVMe device paths: {ERROR}",
                       "ERROR", e.what());
        }
    }
    return driveList;
}

TargetFilter NVMeItemUpdater::applyTargetFilters(
    const std::vector<sdbusplus::message::object_path>& targets)
{
    TargetFilter filter = BaseItemUpdater::applyTargetFilters(targets);
    if (getDevicesToUpdate(filter).empty())
    {
        lg2::warning(
            "No NVMe devices match model {MODEL}, activating as success",
            "MODEL", modelName);
        return {TargetFilterType::UpdateNone, {}};
    }
    return filter;
}

std::string NVMeItemUpdater::getServiceArgs(
    [[maybe_unused]] const std::string& inventoryPath,
    const std::string& imagePath, const std::string& version,
    const TargetFilter& targetFilter) const
{
    std::string deviceList = getDevicesToUpdate(targetFilter);
    if (deviceList.empty())
    {
        lg2::warning("No matching NVMe devices found for model {MODEL}, "
                     "skipping update service launch",
                     "MODEL", modelName);
        return "";
    }

    lg2::info("NVMe update drive name list: {DRIVE_LIST}", "DRIVE_LIST",
              deviceList);

    const std::string requestId = makeNvmeUpdateRequestId();
    const auto requestPath = nvmeRequestFilePath(requestId);
    const auto tmpPath = requestPath.string() + ".tmp";

    try
    {
        std::filesystem::create_directories(requestPath.parent_path());
        {
            std::ofstream envFile(tmpPath, std::ios::trunc);
            if (!envFile)
            {
                lg2::error("Failed to create NVMe update request file {PATH}",
                           "PATH", tmpPath);
                return "";
            }

            envFile << "FW_IMAGE=" << quoteEnvValue(imagePath) << "\n";
            envFile << "FW_VERSION=" << quoteEnvValue(version) << "\n";
            envFile << "FW_PREFIX=" << quoteEnvValue(NVME_INVENTORY_PATH)
                    << "\n";
            envFile << "FW_TARGETS=" << quoteEnvValue(deviceList) << "\n";
        }
        std::filesystem::rename(tmpPath, requestPath);
    }
    catch (const std::exception& e)
    {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        lg2::error("Failed to write NVMe update request file {PATH}: {ERROR}",
                   "PATH", requestPath.string(), "ERROR", e.what());
        return "";
    }

    return requestId;
}

} // namespace updater
} // namespace software
} // namespace nvidia
