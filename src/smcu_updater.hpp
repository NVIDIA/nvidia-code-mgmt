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

#include "../nvidia_igx/orin/src/orin_util.hpp"
#include "base_item_updater.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <algorithm>
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

    /** @brief Tracks the SMCU type detected from the platform at startup */
    std::string detectedPlatformSmcu;

  private:
    /**
     * @brief Detect SMCU type from platform detection
     * @return "ASMCU" (Aurix) or "RSMCU" (Renesas)
     */
    std::string detectPlatformSmcu()
    {
        try
        {
            // Direct SMCU type detection
            std::string cmd =
                "/usr/bin/igx-platform-detection.py --type smcu --no-wait";
            std::string smcuType = nvidia::orin::common::readVersionFile(cmd);

            if (smcuType == "RSMCU")
            {
                lg2::info("Detected SMCU type: Renesas (from SMCU_SOC field)");
                return "RSMCU";
            }
            else if (smcuType == "ASMCU")
            {
                lg2::info("Detected SMCU type: Aurix (from SMCU_SOC field)");
                return "ASMCU";
            }

            // Priority 2: Fallback to platform name detection
            lg2::info("SMCU_SOC field unavailable, falling back to platform "
                      "name detection");

            nvidia::orin::common::Util util;
            std::string platformName = util.getPlatformName();

            if (platformName == "Thor")
            {
                lg2::info(
                    "Detected Thor platform, using Renesas SMCU (fallback)");
                return "RSMCU";
            }
            else if (platformName == "Orin")
            {
                lg2::info(
                    "Detected Orin platform, using Aurix SMCU (fallback)");
                return "ASMCU";
            }
            else
            {
                lg2::error(
                    "Unknown platform: {PLATFORM}. Cannot determine SMCU type",
                    "PLATFORM", platformName.empty() ? "empty" : platformName);
                return "";
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to detect platform SMCU: {ERROR}", "ERROR",
                       e.what());
            return "";
        }
    }

  public:
    /**
     * @brief Construct a new SMCUItemUpdater object
     *
     * @param bus
     */
    SMCUItemUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, SMCU_SUPPORTED_MODEL, SMCU_INVENTORY_IFACE, "SMCU",
                        SMCU_BUSNAME_UPDATER, SMCU_UPDATE_SERVICE, false,
                        SMCU_BUSNAME_INVENTORY)
    {
        detectedPlatformSmcu = detectPlatformSmcu();
    }
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
        // If we already processed an image and know the model, use it
        if (currentModelToken == "ASMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
            return ret;
        }
        else if (currentModelToken == "RSMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
            return ret;
        }

        // Priority 2: Use detectedPlatformSmcu (from startup detection)
        if (!detectedPlatformSmcu.empty())
        {
            if (detectedPlatformSmcu == "ASMCU")
            {
                ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
                return ret;
            }
            else if (detectedPlatformSmcu == "RSMCU")
            {
                ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
                return ret;
            }
        }

        // Fallback: Return both if platform detection failed
        // Fallback: Return both if platform detection failed
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
     *        Resolution order:
     *          1. Model from deviceIds map keyed by the given UUID
     *          2. SMCU type cached from startup platform detection
     *          3. Live platform detection via detectPlatformSmcu()
     *          4. getName() fallback when SMCU type cannot be determined
     */
    std::string getIdProperty(const std::string& uniqueIdentifier) override
    {
        // Priority 1: Check if called with a UUID (after processImage)
        // Extract model from deviceIds map using the UUID
        if (!uniqueIdentifier.empty())
        {
            auto it = deviceIds.find(uniqueIdentifier);
            if (it != deviceIds.end())
            {
                std::string model = std::get<0>(it->second);
                if (model == "ASMCU")
                {
                    return "ASMCU";
                }
                else if (model == "RSMCU")
                {
                    return "RSMCU";
                }
            }
        }

        // Priority 2: Use cached platform detection (from constructor)
        if (!detectedPlatformSmcu.empty())
        {
            return detectedPlatformSmcu;
        }

        // Priority 3: Detect now (called during base constructor)
        std::string smcuType = detectPlatformSmcu();
        if (smcuType == "ASMCU" || smcuType == "RSMCU")
        {
            return smcuType;
        }

        // Fallback: Use generic SMCU
        return getName();
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
