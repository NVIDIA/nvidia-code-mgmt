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
#include "config.h"

#include "base_item_updater.hpp"

#include <filesystem>
#include <tuple>

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief SMCU updater
 * @author
 * @since Jan 12 2024
 */
class SMCUItemUpdater : public BaseItemUpdater
{
    /** @brief Tracks the model detected from the current image being
     *         processed
     */
    std::string currentModelToken;

  public:
    /**
     * @brief Construct a new SMCUItemUpdater object
     *
     * @param bus
     */
    SMCUItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, SMCU_SUPPORTED_MODEL, SMCU_INVENTORY_IFACE, "SMCU",
                        SMCU_BUSNAME_UPDATER, SMCU_UPDATE_SERVICE, false,
                        SMCU_BUSNAME_INVENTORY)
    {}
    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string
        getManufacturer(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {

        // The systemd unit shall be escaped
        std::string args = inventoryPath;
        args += "\\x20";
        args += imagePath;

        try
        {
            const std::string uuid = std::filesystem::path(imagePath)
                                         .parent_path()
                                         .filename()
                                         .string();
            auto mapSMCUEntry = deviceIds.find(uuid);
            if (mapSMCUEntry != deviceIds.end())
            {
                const std::string& model = std::get<0>(mapSMCUEntry->second);
                if (!model.empty())
                {
                    args += "\\x20";
                    args += model; // pass model name as 3rd arg
                }
            }
        }
        catch (...)
        {
            // Ignore and proceed without third arg
        }
        std::replace(args.begin(), args.end(), '/', '-');

        return args;
    }

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        if (currentModelToken == "ASMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
            return ret;
        }
        if (currentModelToken == "RSMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
            return ret;
        }
        // Default: expose both if model not yet known
        ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
        ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
        return ret;
    }

    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief Generate stable IDs per model.
     *        ASMCU  -> Aurix_SMCU
     *        RSMCU  -> Renesas_SMCU
     *        Others -> fallback unique by UUID
     */
    std::string getIdProperty(const std::string& uniqueIdentifier) override
    {
        if (uniqueIdentifier == "ASMCU")
        {
            return "Aurix_SMCU";
        }
        if (uniqueIdentifier == "RSMCU")
        {
            return "Renesas_SMCU";
        }

        std::string model;
        auto it = deviceIds.find(uniqueIdentifier);
        if (it != deviceIds.end())
        {
            model = std::get<0>(it->second);
        }
        if (model == "ASMCU")
        {
            return "Aurix_SMCU";
        }
        if (model == "RSMCU")
        {
            return "Renesas_SMCU";
        }

        // Fallback to unique ID to avoid collisions
        if (uniqueIdentifier.empty())
        {
            return getName();
        }
        std::string id = getName();
        id += "_";
        std::string sanitized = uniqueIdentifier;
        std::replace(sanitized.begin(), sanitized.end(), '/', '_');
        id += sanitized;
        return id;
    }

    int processImage(std::filesystem::path& filePath) override
    {
        try
        {
            const std::string uuid = filePath.parent_path().filename().string();
            auto it = deviceIds.find(uuid);
            if (it != deviceIds.end())
            {
                currentModelToken = std::get<0>(it->second);
            }
            else
            {
                currentModelToken.clear();
            }
        }
        catch (...)
        {
            currentModelToken.clear();
        }
        return BaseItemUpdater::processImage(filePath);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
