// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once
#include "config.h"

#include "base_item_updater.hpp"
#include "usbdfu_components.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief USB DFU recovery item updater (HMC / AST2700)
 *
 * The recovery package carries the preliminary boot bundle and the SPI
 * image as separate components (usbdfu::componentMap).  PLDM extracts them to
 *   <IMG_UPLOAD_DIR>/USBDFURecovery/<UUID>/<component id>/<file>
 * and, when activated, usb-dfu-recovery@<UUID dir>.service is started.  The
 * worker (usb-dfu-recovery) resolves the components from that directory,
 * reads the GPIO strap names from Entity Manager
 * (xyz.openbmc_project.Configuration.USBDFURecovery), performs the recovery
 * and reports progress through the Redfish message registry.  Only the
 * package path crosses the systemd boundary, like USBRCMRecovery.
 */
class USBDFURecovery : public BaseItemUpdater
{
  public:
    explicit USBDFURecovery(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, USBDFU_RECOVERY_SUPPORTED_MODEL,
                        USBDFU_RECOVERY_INVENTORY_IFACE, USBDFU_RECOVERY_NAME,
                        USBDFU_RECOVERY_BUSNAME_UPDATER,
                        USBDFU_RECOVERY_UPDATE_SERVICE, false,
                        USBDFU_RECOVERY_BUSNAME_INVENTORY)
    {}

    std::string getVersion(const std::string& inventoryPath) const override;
    std::string getManufacturer(
        [[maybe_unused]] const std::string& inventoryPath) const override;
    std::string getModel(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Build the systemd instance argument: the package directory only.
     *
     * The unit is usb-dfu-recovery@<escaped path>.service; systemd's %I turns
     * the escaped string back into the path handed to the worker.
     */
    std::string getServiceArgs(
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

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return {std::string(SOFTWARE_OBJPATH) + "/" + USBDFU_RECOVERY_NAME};
    }

    uint32_t getTimeout() override
    {
        return USBDFU_RECOVERY_TIMEOUT;
    }

    bool inventorySupported() override
    {
        return false;
    }

    /**
     * @brief Watch every component subdirectory of the package.
     *
     * PLDM extracts each component of the multi-component package to
     * <UUID>/<component id, decimal>/<file>; the base implementation only
     * watches <UUID>/.
     */
    std::vector<std::filesystem::path> getPathsToMonitor() const override
    {
        std::vector<std::filesystem::path> pathsToMonitor;
        for (const auto& [uuid, _] : deviceIds)
        {
            std::filesystem::path basePath(getImageUploadDir());
            basePath /= uuid;
            for (const auto& component : usbdfu::componentMap)
            {
                pathsToMonitor.push_back(basePath /
                                         std::to_string(component.id));
            }
        }
        if (pathsToMonitor.empty())
        {
            throw std::runtime_error("No " + getName() + " to monitor");
        }
        return pathsToMonitor;
    }

    /**
     * @brief Register the package once its first component file appears.
     *
     * Files live in <UUID>/<component id>/<file>, so the unique identifier is
     * two levels up (the base implementation goes up one level).  The image
     * path handed to the worker is the <UUID> directory.
     */
    int processImage(std::filesystem::path& filePath) override
    {
        const std::filesystem::path packageDir =
            filePath.parent_path().parent_path();

        std::string uniqueIdentifier = packageDir.string();
        boost::replace_all(uniqueIdentifier, getImageUploadDir(), "");

        auto id = getIdProperty(uniqueIdentifier);
        if (id.empty())
        {
            return -1;
        }

        // Every component file triggers inotify; only the first one creates
        // the version object.
        auto it = versions.find(id);
        if (it != versions.end())
        {
            auto currentStatus = it->second->activation();
            if (currentStatus == Version::Status::Activating ||
                currentStatus == Version::Status::Ready)
            {
                return 0;
            }
        }

        auto objPath = std::string{SOFTWARE_OBJPATH} + '/' + id;
        return initiateUpdateImage(objPath, packageDir.string(),
                                   filePath.stem(), id, uniqueIdentifier);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
