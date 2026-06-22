/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
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

#include "orin_util.hpp"

#include <sdbusplus/bus/match.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Chassis/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace nvidia::orin::common;

namespace nvidia::orin::device
{

using OrinInherit = sdbusplus::server::object::object<
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
 * @class Orin
 */
class Orin : public OrinInherit, public Util
{
  public:
    Orin() = delete;
    Orin(const Orin&) = delete;
    Orin(Orin&&) = delete;
    Orin& operator=(const Orin&) = delete;
    Orin& operator=(Orin&&) = delete;
    ~Orin() = default;

    Orin(sdbusplus::bus_t& bus, const std::string& objPath) :
        OrinInherit(bus, (objPath).c_str()), bus(bus), inventoryPath(objPath)
    {
        sdbusplus::xyz::openbmc_project::Inventory::server::Item::prettyName(
            "ORIN");

        registerSoftwareVersion(bus, objPath);
    }

    void registerSoftwareVersion(sdbusplus::bus_t& bus,
                                 const std::string& /* ifPath */)
    {
        std::string swpath = SW_INV_PATH;

        // Get platform name dynamically from IGX host
        std::string platformName = getPlatformName();

        if (platformName == "Thor")
        {
            swpath += "/IGX_Thor";
        }
        else if (platformName == "Orin")
        {
            swpath += "/IGX_Orin";
        }
        else
        {
            swpath += "/IGX_Host";
        }

        std::string orinVersion = getVersion();
        VersionObj =
            std::make_unique<VersionInterface>(bus, swpath, orinVersion);
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

} // namespace nvidia::orin::device
