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

#include "ap_resource.hpp"
#include "dbusutils.hpp"
#include "erot_resource.hpp"
#include "gpio_resource.hpp"
#include "gpu_resource.hpp"
#include "mctp_discovery_resource.hpp"
#include "mctp_endpoint_discovery.hpp"
#include "mctp_vdm_helper.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/event.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface =
    "xyz.openbmc_project.Configuration.OCPRecovery";
constexpr auto glacierCrisisObjInterface =
    "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto gpioObjInterface =
    "xyz.openbmc_project.Configuration.GPIORecovery";
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/xyz/openbmc_project/inventory/system/";
constexpr auto i2cInterface =
    "xyz.openbmc_project.Inventory.Decorator.I2CDevice";
constexpr auto uuidInterface = "xyz.openbmc_project.Common.UUID";

using namespace phosphor::logging;
using namespace nvidia::software::updater;

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

std::string getSoftwareDBusObjectPath(const std::filesystem::path& path)
{
    std::string name = path.filename();
    return "/xyz/openbmc_project/software/" + name;
}

std::string getChassisObjPath(const std::string& chassisName)
{
    return "/xyz/openbmc_project/inventory/system/chassis/" + chassisName;
}

std::string getString(const InterfaceMap& interfaces,
                      const Interface& interface, const Property& property)
{
    try
    {
        return std::get<std::string>(interfaces.at(interface).at(property));
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return {};
    }
}

uint64_t getUint64(const InterfaceMap& interfaces, const Interface& interface,
                   const Property& property)
{
    try
    {
        return std::get<uint64_t>(interfaces.at(interface).at(property));
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return {};
    }
}

bool getBool(const InterfaceMap& interfaces, const Interface& interface,
             const Property& property)
{
    try
    {
        return std::get<bool>(interfaces.at(interface).at(property));
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return {};
    }
}

int main()
{
    auto& bus = getBus();
    sdbusplus::server::manager_t mgr{bus, fwStatusObjManager};

    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr instanceIdMgr;

    using namespace mctp_vdm;

    // MCTP VDM requester handler
    requester::Handler<requester::Request> reqHandler(event, instanceIdMgr,
                                                      sockManager);

    mctp_socket::Handler sockHandler(event, reqHandler, sockManager);

    auto mctpVdmHelper = std::make_shared<MCTPVdmHelper>(
        bus, reqHandler, sockHandler, instanceIdMgr);

    std::unique_ptr<MctpDiscovery> mctpDiscoveryHandler =
        std::make_unique<MctpDiscovery>(
            bus, sockHandler,
            std::initializer_list<mctp_vdm::MctpDiscoveryHandlerIntf*>{
                mctpVdmHelper.get()});

    bus.request_name(fwStatusService);

    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);

    std::vector<std::unique_ptr<BaseResource>> resources;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        const auto objPath =
            getSoftwareDBusObjectPath(std::string(emObjectPath));
        if (interfaces.contains(ocpObjInterface))
        {
            lg2::info("Found OCP recovery config Object: {PATH}", "PATH",
                      emObjectPath);
            const auto i2cBus =
                getUint64(interfaces, ocpObjInterface, "I2CBus");
            const auto i2cAddress =
                getUint64(interfaces, ocpObjInterface, "I2CAddress");
            const auto uuid =
                getString(interfaces, ocpObjInterface, "MctpUUID");
            const auto chassisName =
                getString(interfaces, ocpObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);
            resources.push_back(std::make_unique<GpuResource>(
                bus, objPath, chassisObjPath, i2cBus, i2cAddress, uuid));
        }
        else if (interfaces.contains(glacierCrisisObjInterface))
        {
            lg2::info("Found Glacier Crisis recovery config Object: {PATH}",
                      "PATH", emObjectPath);
            const auto isRecoverable =
                getBool(interfaces, glacierCrisisObjInterface, "isRecoverable");

            uint64_t i2cBus, i2cAddress;
            if (isRecoverable)
            {
                i2cBus =
                    getUint64(interfaces, glacierCrisisObjInterface, "I2CBus");
                i2cAddress = getUint64(interfaces, glacierCrisisObjInterface,
                                       "I2CAddress");
            }
            const auto uuid =
                getString(interfaces, glacierCrisisObjInterface, "MctpUUID");
            const auto apBootStatusType = getString(
                interfaces, glacierCrisisObjInterface, "APBootStatusType");
            const auto chassisName =
                getString(interfaces, glacierCrisisObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);
            if (!apBootStatusType.empty())
            {
                lg2::info(
                    "Found AP config on Glacier Crisis recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                if (isRecoverable)
                {
                    const auto apEid = getUint64(
                        interfaces, glacierCrisisObjInterface, "APEID");
                    const auto apName = getString(
                        interfaces, glacierCrisisObjInterface, "APName");
                    const auto apObjPath = getSoftwareDBusObjectPath(apName);
                    resources.push_back(std::make_unique<ERoTResource>(
                        bus, objPath, i2cBus, i2cAddress, uuid, apEid,
                        chassisObjPath, apObjPath, isRecoverable,
                        mctpVdmHelper));
                }
                else
                {
                    resources.push_back(std::make_unique<ERoTResource>(
                        bus, objPath, uuid, chassisObjPath, isRecoverable,
                        mctpVdmHelper));
                }
            }
        }
        else if (interfaces.contains(gpioObjInterface))
        {
            const auto uuid =
                getString(interfaces, gpioObjInterface, "MctpUUID");
            const auto gpio = getString(interfaces, gpioObjInterface, "GPIO");
            const auto isErot = getBool(interfaces, gpioObjInterface, "IsERoT");
            if (isErot)
            {
                lg2::info("Found GPIO recovery Object (ERoT): {PATH}", "PATH",
                          emObjectPath);
                const auto i2cBus =
                    getUint64(interfaces, gpioObjInterface, "I2CBus");
                const auto i2cAddress =
                    getUint64(interfaces, gpioObjInterface, "I2CAddress");
                const auto target =
                    getString(interfaces, gpioObjInterface, "Target");
                resources.push_back(std::make_unique<GPIOResource>(
                    bus, objPath, event, i2cBus, i2cAddress, uuid, gpio,
                    target));
            }
            else
            {
                lg2::info("Found GPIO recovery Object (AP): {PATH}", "PATH",
                          emObjectPath);
                const auto risingTarget =
                    getString(interfaces, gpioObjInterface, "RisingTarget");
                const auto fallingTarget =
                    getString(interfaces, gpioObjInterface, "FallingTarget");
                const auto polarity =
                    getString(interfaces, gpioObjInterface, "Polarity");
                resources.push_back(std::make_unique<GPIOResource>(
                    bus, objPath, event, uuid, gpio, risingTarget,
                    fallingTarget, polarity));
            }
        }
    }

    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    event.loop();
}
