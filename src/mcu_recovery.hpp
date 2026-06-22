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

namespace nvidia
{
namespace software
{
namespace updater
{
class MCURecovery : public BaseItemUpdater
{
  private:
    std::string targetName;

  public:
    /**
     * @brief Construct a new MCU Recovery Item Updater object
     *
     * @param bus dbus reference
     * @param model model name
     * @param target target name of the MCU
     */
    MCURecovery(sdbusplus::bus_t& bus, const std::string& model,
                const std::string& target) :
        BaseItemUpdater(
            bus, model, MCU_RECOVERY_INVENTORY_IFACE, MCU_RECOVERY_NAME,
            MCU_RECOVERY_BUSNAME_UPDATER + target, MCU_RECOVERY_UPDATE_SERVICE,
            false, MCU_RECOVERY_BUSNAME_INVENTORY),
        targetName(target)
    {
        lg2::info(
            "MCU Recovery Item Updater - Model: {MODEL}, Target: {TARGET}",
            "MODEL", model, "TARGET", target);
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
     * @param forceUpdate
     * @return std::string
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
        args += "\\x20";
        args += targetName;
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
        std::string invPath =
            std::string(SOFTWARE_OBJPATH) + "/" + MCU_RECOVERY_NAME;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout for glacier recovery
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return MCU_RECOVERY_TIMEOUT;
    }

    std::string
        getIdProperty([[maybe_unused]] const std::string& identifier) override
    {
        return std::string(MCU_RECOVERY_NAME) + "/" + targetName;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for mcu recovery inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
