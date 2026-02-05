/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2025 NVIDIA CORPORATION &
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

#include "fstream"

namespace nvidia
{
namespace software
{
namespace updater
{

class NVSwitchRecoveryUpdater : public BaseItemUpdater
{
  public:
    NVSwitchRecoveryUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(
            bus, NVSWITCH_RECOVERY_SUPPORTED_MODEL,
            NVSWITCH_RECOVERY_INVENTORY_IFACE, NVSWITCH_RECOVERY_NAME,
            NVSWITCH_RECOVERY_BUSNAME_UPDATER, NVSWITCH_RECOVERY_UPDATE_SERVICE,
            false, NVSWITCH_RECOVERY_BUSNAME_INVENTORY)
    {}

    /**
     * @brief Get the Version
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getManufacturer(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model
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
    std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {
        std::string args = imagePath;
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
            std::string(SOFTWARE_OBJPATH) + "/" + NVSWITCH_RECOVERY_NAME;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout from config file
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return NVSWITCH_RECOVERY_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for nvswitch recovery inventory check is not required
     */
    bool inventorySupported() override
    {
        return false;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
