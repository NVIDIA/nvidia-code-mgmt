/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

/**
 * @brief ORIN updater
 * @author
 * @since Dec 20 2023
 */
class ORINItemUpdater : public BaseItemUpdater
{
  public:
    /**
     * @brief Construct a new ORINItemUpdater object
     *
     * @param bus
     */
    ORINItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, ORIN_SUPPORTED_MODEL, ORIN_INVENTORY_IFACE, "ORIN",
                        ORIN_BUSNAME_UPDATER, ORIN_UPDATE_SERVICE, false,
                        ORIN_BUSNAME_INVENTORY)
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
    virtual std::string
        getServiceArgs([[maybe_unused]] const std::string& inventoryPath,
                       const std::string& imagePath,
                       [[maybe_unused]] const std::string& version,
                       [[maybe_unused]] const TargetFilter &targetFilter,
                       [[maybe_unused]] const bool forceUpdate) const override
    {

        // The systemd unit shall be escaped
        std::string args = inventoryPath;
        args += "\\x20";
        args += imagePath;
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
            std::string(SOFTWARE_OBJPATH) + "/ORIN";
        ret.emplace_back(invPath);
        return ret;
    }

    bool inventorySupported() override
    {
        return false; // default is supported
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
