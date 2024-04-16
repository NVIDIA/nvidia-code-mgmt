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
#include "fstream"
#include <xyz/openbmc_project/State/Host/server.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{

namespace StateServer = sdbusplus::xyz::openbmc_project::State::server;

class SwitchtecFuse : public BaseItemUpdater
{   
  std::unique_ptr<SoftwareVersion> softwareVersionObj;
  //sdbusplus::bus::match_t propertiesChangedSignalCurrentHostState;
  sdbusplus::bus::match::match _match;
  public:
    SwitchtecFuse(sdbusplus::bus::bus& bus) :
             BaseItemUpdater(bus, SWITCHTEC_SUPPORTED_MODEL, SWITCHTEC_INVENTORY_IFACE, "PCIE_SWITCH_FUSE",
                        SWITCHTEC_BUSNAME_UPDATER, SWITCHTEC_FUSE_SERVICE, false, SWITCHTEC_BUSNAME_INVENTORY),
             _match(bus, sdbusplus::bus::match::rules::propertiesChanged("/xyz/openbmc_project/state/host0",
                                                                         "xyz.openbmc_project.State.Host"),
                    [this](auto& msg) {
                      std::string intfName;
                      std::map<std::string, std::variant<std::string>> msgData;
                      msg.read(intfName, msgData);
                      // Check if it was the Value property that changed.
                      auto valPropMap = msgData.find("CurrentHostState");
                      if (valPropMap != msgData.end())
                      {
                        StateServer::Host::HostState currentHostState =
                        StateServer::Host::convertHostStateFromString(
                        std::get<std::string>(valPropMap->second));
                        if (currentHostState == StateServer::Host::HostState::Running)
                        {
                          getVersion("");
                        }
                      }
                    })
    {
        auto objPath = std::string(SOFTWARE_OBJPATH) + "/PCIE_SWITCH_FUSE";
        createInventory(bus, objPath);
    }

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion([
        [maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getManufacturer([
        [maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel([
        [maybe_unused]] const std::string& inventoryPath) const override;

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
                       const std::string& version,
                       [[maybe_unused]] const TargetFilter &targetFilter,
                       [[maybe_unused]] const bool forceUpdate) const override
    {
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += version;
        args += "\\x20";
        args += "SWITCHTEC_PCIE_SWITCH";
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
            std::string(SOFTWARE_OBJPATH) + "/Switchtec";
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout from config file for retimer
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return SWITCHTEC_FUSE_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false */
    bool inventorySupported() override
    {
        return false;
    }

    /**
     * @brief create version interface for required non-pldm devices
     * @param bus
     * @param objpath
     * @param versionId
     */
    void createInventory(sdbusplus::bus::bus& bus,
                                const std::string& objPath)
    {
        getVersion("");
        softwareVersionObj = std::make_unique<SoftwareVersion>(bus, objPath);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
