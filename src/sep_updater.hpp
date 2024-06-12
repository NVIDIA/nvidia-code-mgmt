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

class SEPItemUpdater : public BaseItemUpdater
{
  public:
    /**
     * @brief Construct a new SEPItemUpdater object
     *
     * @param bus
     */
    SEPItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, SEP_SUPPORTED_MODEL, SEP_INVENTORY_IFACE, "SEP",
                        SEP_BUSNAME_UPDATER, SEP_UPDATE_SERVICE, false,
                        SEP_BUSNAME_INVENTORY)
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
                       [[maybe_unused]] const TargetFilter &targetFilter) const override
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
            std::string(SOFTWARE_OBJPATH) + "/SEP";
        ret.emplace_back(invPath);
        return ret;
    }

    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief Override the default timeout to SEP timeout.
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return SEP_TIMEOUT;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
