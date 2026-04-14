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

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <filesystem>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

class NVMeDevice
{
    std::string name, inventoryPath, devicePath;

  public:
    /**
     * @brief Construct a new NVMeDevice object
     *
     * @param objPath inventory object path
     * @param devPath device path (e.g., /dev/nvme0n1)
     * @param name device name
     */
    NVMeDevice(const std::string& objPath, const std::string& devPath,
               const std::string& name) :
        name(name), inventoryPath(objPath), devicePath(devPath)
    {}

    /**
     * @brief Get the Inventory Path object
     *
     * @return const std::string&
     */
    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

    /**
     * @brief Get the Device Path object
     *
     * @return const std::string&
     */
    const std::string& getDevicePath() const
    {
        return devicePath;
    }

    /**
     * @brief Get the device name
     *
     * @return const std::string&
     */
    const std::string& getName() const
    {
        return name;
    }
};

/**
 * @brief concrete class for NVMe drive firmware updates
 * @author
 * @since 2024
 */
class NVMeItemUpdater : public BaseItemUpdater
{
    const std::string targetName;
    const std::string objPath;
    const std::string modelName;

  public:
    /**
     * @brief Construct a new NVMeItemUpdater object
     *
     * @param bus dbus reference
     * @param together update everything together
     */
    NVMeItemUpdater(sdbusplus::bus::bus& bus, bool together,
                    const std::string& model, const std::string& target) :
        BaseItemUpdater(bus, model, NVME_INVENTORY_IFACE, NVME_NAME,
                        NVME_BUSNAME_UPDATER + target, NVME_UPDATE_SERVICE,
                        together, NVME_BUSNAME_INVENTORY),
        targetName(target),
        objPath(std::string(SOFTWARE_OBJPATH) + "/" + std::string(NVME_NAME) +
                (target.empty() ? "" : "_" + target)),
        modelName(model)
    {}

    /**
     * @brief Get the Name object (overridden to include target name)
     *
     * @return std::string
     */
    std::string getName() const override
    {
        return std::string(NVME_NAME) +
               (targetName.empty() ? "" : "_" + targetName);
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
    std::string getManufacturer(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath, const std::string& version,
                       const TargetFilter& targetFilter) const override;

    /**
     * @brief Get devices to update based on target filters
     *
     * @param targetFilter
     * @return std::string representing device IDs to update
     */
    std::string getDevicesToUpdate(const TargetFilter& targetFilter) const;

    /**
     * @brief Check if path is a valid NVMe device
     *
     * @param p inventory path
     * @return true if valid
     * @return false if invalid
     */
    bool pathIsValidDevice(std::string& p) override
    {
        constexpr const char* interface =
            "xyz.openbmc_project.Inventory.Item.StorageController";
        try
        {

            auto mapper = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                              MAPPER_INTERFACE, "GetObject");
            mapper.append(p.c_str(), std::vector<std::string>({interface}));
            auto mapperResponseMsg = bus.call(mapper);

            std::vector<std::pair<std::string, std::vector<std::string>>>
                mapperResponse;
            mapperResponseMsg.read(mapperResponse);

            // If we get a response and it's not empty, the path is valid
            bool isValid = !mapperResponse.empty();
            return isValid;
        }
        catch (const sdbusplus::exception::SdBusError& ex)
        {
            // If GetObject call fails, the path is not valid
            lg2::error(
                "pathIsValidDevice: Path {PATH} is INVALID - D-Bus error: {ERROR}",
                "PATH", p, "ERROR", ex.what());
            return false;
        }
    }

    /**
     * @brief Sentinel returned by getItemUpdaterInventoryPaths() when no drives
     *        match the expected model.  Its presence prevents startActivation()
     *        from failing on the empty-list guard; applyTargetFilters() detects
     *        it and returns UpdateNone so the activation finishes as Active
     *        (good signal to PLDM, no failed task state).
     *
     *        readDeviceDetails() is overridden below to skip this value so
     *        readExistingFirmWare() never registers a D-Bus match rule against
     *        an invalid path.
     */
    static constexpr auto nvmeNoMatchSentinel = "__nvme_no_matching_devices__";

    /**
     * @brief Get inventory paths for NVMe devices, filtered by model.
     *
     *        Returns only paths whose model matches the configured modelName.
     *        Returns a single sentinel path when no drives match so that
     *        startActivation() does not immediately return Status::Failed;
     *        applyTargetFilters() will short-circuit to UpdateNone instead.
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        // Extract model name from composite "Manufacturer:Model:UUID" format
        std::string expectedModel = modelName;
        auto firstColon = modelName.find(':');
        if (firstColon != std::string::npos)
        {
            auto secondColon = modelName.find(':', firstColon + 1);
            expectedModel = (secondColon != std::string::npos)
                                ? modelName.substr(firstColon + 1,
                                                   secondColon - firstColon - 1)
                                : modelName.substr(firstColon + 1);
        }

        std::vector<std::string> matchingPaths;
        for (const auto& path : BaseItemUpdater::getItemUpdaterInventoryPaths())
        {
            if (getModel(path) == expectedModel)
            {
                matchingPaths.push_back(path);
            }
        }
        if (matchingPaths.empty())
        {
            lg2::warning("No NVMe drives found matching model {MODEL}", "MODEL",
                         expectedModel);
            return {std::string(nvmeNoMatchSentinel)};
        }
        return matchingPaths;
    }

    /**
     * @brief Skip readDeviceDetails for the no-match sentinel path.
     *
     *        readExistingFirmWare() iterates over
     * getItemUpdaterInventoryPaths() and calls readDeviceDetails() for each
     * entry.  When the sentinel is present, the base implementation would try
     * to register a D-Bus match rule using the sentinel string as an object
     * path.  Because the sentinel is not a valid D-Bus path (no leading '/'),
     * sd_bus_match rejects it with InvalidArgs and terminates the process.
     * Return early to avoid that.
     */
    void readDeviceDetails(std::string& p) override
    {
        if (p == nvmeNoMatchSentinel)
        {
            return;
        }
        BaseItemUpdater::readDeviceDetails(p);
    }

    /**
     * @brief Apply target filters, short-circuiting to UpdateNone when no
     *        NVMe devices match the firmware model.  UpdateNone causes
     *        startActivation() to call finishActivation() → Status::Active,
     *        sending a successful completion signal to PLDM without marking
     *        the task as failed.
     *
     * @param targets
     * @return TargetFilter
     */
    TargetFilter applyTargetFilters(
        const std::vector<sdbusplus::message::object_path>& targets) override;

    /**
     * @brief Get D-Bus service name
     *
     * @param path object path
     * @param interface interface name
     * @return std::string
     */
    std::string getDbusService(const std::string& /* path */,
                               const std::string& /* interface */) override
    {
        return NVME_BUSNAME_INVENTORY;
    }

    /**
     * @brief Get timeout for NVMe updates
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return NVME_UPDATE_TIMEOUT;
    }

    /**
     * @brief Check if inventory is supported
     *
     * @return false for NVMe (inventory check not required)
     */
    bool inventorySupported() override
    {
        return false;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia