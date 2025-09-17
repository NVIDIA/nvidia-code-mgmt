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

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

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
        ret = getProperty<std::string>(
            NVME_BUSNAME_INVENTORY, inventoryPath.c_str(), ASSET_IFACE, MODEL);

        // Get the second word from the model string (space-separated)
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
 * @brief Get retimer the devices to update object based on target filters
 *
 * @param targetFilter
 * @return std::bitset representing which retimers to update. At any bit
 *                     1 represents that the retimer is to be updated
 *                     and 0 for skipping update to that retimer
 */
std::string
    NVMeItemUpdater::getDevicesToUpdate(const TargetFilter& targetFilter) const
{
    std::string eidList;
    if (targetFilter.type == TargetFilterType::UpdateSelected)
    {
        for (auto& target : targetFilter.targets)
        {
            uint deviceId;
            int ret =
                std::sscanf(target.c_str(), FW_NVME_NAME_FORMAT, &deviceId);
            if (ret > 0)
            {
                eidList += std::to_string(deviceId) + "\\x20";
            }
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
                // Check if this object implements the Drive interface
                if (interfaces.find(NVME_INVENTORY_IFACE) == interfaces.end())
                {
                    continue;
                }
                std::string pathStr = path.str;
                uint deviceId;
                int ret = std::sscanf(pathStr.c_str(), NVME_INV_PATH_FORMAT,
                                      &deviceId);
                if (ret > 0)
                {
                    eidList += std::to_string(deviceId) + "\\x20";
                }
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to get NVMe device paths for UpdateAll: {ERROR}",
                       "ERROR", e.what());
        }
    }
    lg2::info("eidList: {EID}", "EID", eidList);
    return eidList;
}

std::string NVMeItemUpdater::getServiceArgs(
    [[maybe_unused]] const std::string& inventoryPath,
    const std::string& imagePath, const std::string& version,
    const TargetFilter& targetFilter) const
{
    std::string args = "";
    args += "\\x20";
    args += imagePath; // image path
    args += "\\x20";
    args += version; // version string for message registry
    args += "\\x20";
    args += NVME_INVENTORY_PATH; // path for message registry
    args += "\\x20";
    args += getDevicesToUpdate(targetFilter); // collect eid list

    std::replace(args.begin(), args.end(), '/', '-');
    return args;
}

} // namespace updater
} // namespace software
} // namespace nvidia