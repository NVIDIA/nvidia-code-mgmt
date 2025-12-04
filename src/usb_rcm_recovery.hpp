/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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
#include <sstream>

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief USB RCM Recovery item updater for Vera/TB500 devices
 * @details Handles recovery of NVIDIA Vera/TB500 devices via USB RCM protocol
 */
class USBRCMRecovery : public BaseItemUpdater
{

  public:
    /**
     * @brief Construct a new USB RCM Recovery ItemUpdater object
     *
     * @param bus D-Bus connection
     */
    USBRCMRecovery(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, USBRCM_RECOVERY_SUPPORTED_MODEL,
                        USBRCM_RECOVERY_INVENTORY_IFACE, USBRCM_RECOVERY_NAME,
                        USBRCM_RECOVERY_BUSNAME_UPDATER,
                        USBRCM_RECOVERY_UPDATE_SERVICE, false,
                        USBRCM_RECOVERY_BUSNAME_INVENTORY)
    {}

    /**
     * @brief Get the Version
     *
     * @param inventoryPath Inventory path (unused for recovery)
     * @return std::string Empty string (recovery doesn't report version)
     */
    std::string getVersion(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer
     *
     * @param inventoryPath Inventory path (unused)
     * @return std::string Manufacturer name
     */
    std::string
        getManufacturer(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model
     *
     * @param inventoryPath Inventory path (unused)
     * @return std::string Model name
     */
    std::string getModel(const std::string& inventoryPath) const override;

    /**
     * @brief Get service arguments for systemd service invocation
     *
     * @param inventoryPath Inventory path (unused)
     * @param imagePath Base path containing component image directories
     * @param version Version string (unused)
     * @param targetFilter Target filter (unused)
     * @return std::string Escaped arguments for systemd unit
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {
        // The systemd unit shall be escaped
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string> List of inventory paths
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        std::string invPath =
            std::string(SOFTWARE_OBJPATH) + "/" + USBRCM_RECOVERY_NAME;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout for USB RCM recovery
     *
     * @return uint32_t Timeout in seconds
     */
    uint32_t getTimeout() override
    {
        return USBRCM_RECOVERY_TIMEOUT;
    }

    /**
     * @brief Check if inventory is supported
     * @details For USB RCM recovery, inventory check is not required
     *
     * @return false Inventory is not supported
     */
    bool inventorySupported() override
    {
        return false;
    }

    /**
     * @brief Get the paths to monitor for inotify
     * @details USB RCM watches each component subdirectory for file writes
     *          PLDM extracts components to: UUID/2/, UUID/517/, UUID/516/, etc.
     *
     * @return std::vector<std::filesystem::path> Paths to watch
     */
    virtual std::vector<std::filesystem::path>
        getPathsToMonitor() const override
    {
        std::vector<std::filesystem::path> pathsToMonitor{};

        static const std::vector<uint16_t> componentIds = {
            0x2, 0x3, 0x205, 0x204, 0x206, 0x207, 0x208, 0x209, 0x20A};

        for (const auto& [uuid, _] : deviceIds)
        {
            std::filesystem::path basePathToWatch(getImageUploadDir());
            basePathToWatch /= uuid;

            // Watch each component subdirectory where PLDM extracts files
            for (const auto& compId : componentIds)
            {
                std::filesystem::path componentPath = basePathToWatch;
                componentPath /=
                    std::to_string(compId); // Decimal: 0x205 → "517"
                pathsToMonitor.push_back(componentPath);
            }
        }

        if (pathsToMonitor.size() < 1)
        {
            throw std::runtime_error("No " + getName() + " to monitor");
        }

        return pathsToMonitor;
    }

    /**
     * @brief Process images from component subdirectories
     * @details Override needed because files are in UUID/COMPID/file structure
     *          Base implementation only goes up one level (.parent_path())
     *          We need to go up TWO levels (.parent_path().parent_path())
     *
     * @param filePath Path to the image file
     * @return int 0 on success, -1 on failure
     */
    virtual int processImage(std::filesystem::path& filePath) override
    {

        std::string uniqueIdentifier =
            filePath.parent_path().parent_path().string();
        boost::replace_all(uniqueIdentifier, getImageUploadDir(), "");

        auto id = getIdProperty(uniqueIdentifier);
        if (id == "")
        {
            return -1;
        }

        auto it = versions.find(id);
        if (it != versions.end())
        {
            auto currentStatus = it->second->activation();

            if (currentStatus == Version::Status::Activating)
            {
                return 0;
            }

            if (currentStatus == Version::Status::Ready)
            {
                return 0;
            }
        }

        auto objPath = std::string{SOFTWARE_OBJPATH} + '/' + id;
        return initiateUpdateImage(
            objPath, filePath.parent_path().parent_path().string(),
            filePath.stem(), id, uniqueIdentifier);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
