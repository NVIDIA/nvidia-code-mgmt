/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "igx_fuse_util.hpp"

#include <sdbusplus/bus/match.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Chassis/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace nvidia::igxfuse::common;

namespace nvidia::igxfuse::device
{

using IgxFuseInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Chassis,
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset>;

using VersionObject = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Version>;

using PropertyType = std::variant<std::string, bool>;

using Properties = std::map<std::string, PropertyType>;

class VersionInterface : public VersionObject
{
  public:
    /**
     * @brief Construct a new VersionInterfaceobject
     *
     * @param bus
     * @param path
     * @param fwversion
     */
    VersionInterface(sdbusplus::bus_t& bus, const std::string& path,
                     std::string& fwversion) :
        VersionObject(bus, path.c_str(), action::emit_interface_added)
    {
        version(fwversion);
    }
};

/**
 * @class IgxFuse
 */
class IgxFuse : public IgxFuseInherit, public Util
{
  public:
    IgxFuse() = delete;
    IgxFuse(const IgxFuse&) = delete;
    IgxFuse(IgxFuse&&) = delete;
    IgxFuse& operator=(const IgxFuse&) = delete;
    IgxFuse& operator=(IgxFuse&&) = delete;
    ~IgxFuse() = default;

    IgxFuse(sdbusplus::bus_t& bus, const std::string& objPath) :
        IgxFuseInherit(bus, (objPath).c_str()), bus(bus), inventoryPath(objPath)
    {
        sdbusplus::xyz::openbmc_project::Inventory::server::Item::prettyName(
            "IGXFUSE");

        registerSoftwareVersion(bus, objPath);
    }

    void registerSoftwareVersion(sdbusplus::bus_t& bus,
                                 const std::string& ifPath)
    {
        std::string swpath = SW_INV_PATH;
        std::string fName = std::filesystem::path(ifPath).filename().string();
        swpath += "/" + fName;
        std::string igxfuseVersion = getVersion();
        VersionObj =
            std::make_unique<VersionInterface>(bus, swpath, igxfuseVersion);
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

  private:
    /** @brief systemd bus member */
    sdbusplus::bus_t& bus;
    std::string inventoryPath;
    std::unique_ptr<VersionInterface> VersionObj;
};

} // namespace nvidia::igxfuse::device
