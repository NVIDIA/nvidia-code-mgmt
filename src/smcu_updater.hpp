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

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <filesystem>
#include <tuple>
#include <variant>

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
     * @brief Detect platform SMCU type using D-Bus FRU query
     *
     * @return std::string "ASMCU" for Orin, "RSMCU" for Thor, "" if unknown
     */
    std::string detectPlatformSmcu()
    {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto method = bus.new_method_call(
                "xyz.openbmc_project.FruDevice",
                "/xyz/openbmc_project/FruDevice/P3809",
                "org.freedesktop.DBus.Properties", "Get");
            method.append("xyz.openbmc_project.FruDevice",
                          "PRODUCT_PRODUCT_NAME");

            auto reply = bus.call(method);
            std::variant<std::string> productNameVariant;
            reply.read(productNameVariant);
            std::string productName =
                std::get<std::string>(productNameVariant);

            if (productName == "P5840" || productName == "P5940")
            {
                lg2::info(
                    "Detected Thor platform ({PRODUCT}), using Renesas SMCU",
                    "PRODUCT", productName);
                return "RSMCU";
            }
            else if (productName == "P3840" || productName == "P3940")
            {
                lg2::info(
                    "Detected Orin platform ({PRODUCT}), using Aurix SMCU",
                    "PRODUCT", productName);
                return "ASMCU";
            }
            else
            {
                lg2::warning("Unknown platform product name: {PRODUCT}, will "
                             "expose both SMCU types",
                             "PRODUCT", productName);
                return "";
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to detect platform: {ERROR}, will expose both "
                       "SMCU types",
                       "ERROR", e.what());
            return "";
        }
    }

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
        // Priority 1: Use model token from firmware image if available
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
        // Priority 2: Use platform-detected SMCU type if available
        if (!detectedPlatformSmcu.empty())
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/" +
                             detectedPlatformSmcu);
            return ret;
        }
        // Priority 3 (fallback): expose both if detection unavailable
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
