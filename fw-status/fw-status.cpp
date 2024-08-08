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

#include "gpu_resource.hpp"
#include "erot_resource.hpp"
#include "ap_resource.hpp"

#include "dbusutils.hpp"

#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>

#include <sdbusplus/bus.hpp>
#include <phosphor-logging/lg2.hpp>
#include "mctp_vdm_helper.hpp"
#include "mctp_discovery_resource.hpp"
#include "mctp_endpoint_discovery.hpp"
#include <sdeventplus/event.hpp>


#include <iostream>
#include <string_view>
#include <filesystem>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface = "xyz.openbmc_project.Configuration.OCPRecovery";
constexpr auto glacierCrisisObjInterface = "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/xyz/openbmc_project/inventory/system/";
constexpr auto i2cInterface = "xyz.openbmc_project.Inventory.Decorator.I2CDevice";
constexpr auto uuidInterface = "xyz.openbmc_project.Common.UUID";

using namespace phosphor::logging;

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

std::pair<uint32_t, uint32_t> getI2CBusAndAddress(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto i2cBus = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CBus");
    auto i2cAddress = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CAddress");

    return {i2cBus, i2cAddress};
}

std::string getUUID(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto uuid = dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
           interface.c_str(), "MctpUUID");
    return uuid;
}

std::string getChassisName(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto chassisName = dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
           interface.c_str(), "ChassisName");
    return chassisName;
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

uint8_t getEid(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto eid = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "APEID");

    return eid;
}

std::string getBootStatusType(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    std::string bootStatusType{};
    try
    {
        bootStatusType = dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
                interface.c_str(), "APBootStatusType");
    }
    catch (std::exception& e)
    {
        return {};
    }

    return bootStatusType;
}

std::string getApName(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto bootStatusType = dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
            interface.c_str(), "APName");

    return bootStatusType;
}

bool isFwRecoverable(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto bootStatusType = dbusUtil.getProperty<bool>(entityManagerService, objPath.c_str(),
            interface.c_str(), "isRecoverable");

    return bootStatusType;
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
    const auto managedObjects = dbusUtil.getManagedObjects(entityManagerService, entityManagerObjManager);

    std::vector<std::unique_ptr<BaseResource>> resources;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(ocpObjInterface))
        {
            lg2::info("Found OCP recovery config Object: {PATH}", "PATH", emObjectPath);
            const auto [i2cBus, i2cAddress] = getI2CBusAndAddress(emObjectPath, ocpObjInterface);
            const auto uuid = getUUID(emObjectPath, ocpObjInterface);
            const auto objPath = getSoftwareDBusObjectPath(std::string(emObjectPath));
            const auto chassisName = getChassisName(emObjectPath, ocpObjInterface);
            const auto chassisObjPath = getChassisObjPath(chassisName);
            resources.push_back(std::make_unique<GpuResource>(bus, objPath, chassisObjPath, i2cBus, i2cAddress, uuid));
        }
        else if (interfaces.contains(glacierCrisisObjInterface))
        {
            lg2::info("Found Glacier Crisis recovery config Object: {PATH}", "PATH", emObjectPath);
            const auto [i2cBus, i2cAddress] = getI2CBusAndAddress(emObjectPath, glacierCrisisObjInterface);
            const auto uuid = getUUID(emObjectPath, glacierCrisisObjInterface);
            const auto objPath = getSoftwareDBusObjectPath(std::string(emObjectPath));
            const auto apBootStatusType = getBootStatusType(emObjectPath, glacierCrisisObjInterface);
            if (!apBootStatusType.empty())
            {
                lg2::info("Found AP config on Glacier Crisis recovery config Object: {PATH}", "PATH", emObjectPath);
                const auto apEid = getEid(emObjectPath, glacierCrisisObjInterface);
                const auto apName = getApName(emObjectPath, glacierCrisisObjInterface);
                const auto chassisName = getChassisName(emObjectPath, glacierCrisisObjInterface);
                const auto chassisObjPath = getChassisObjPath(chassisName);
                const auto apObjPath = getSoftwareDBusObjectPath(apName);
                const auto isRecoverable = isFwRecoverable(emObjectPath, glacierCrisisObjInterface);
                resources.push_back(std::make_unique<ERoTResource>(bus, objPath, i2cBus, i2cAddress, uuid, apEid, chassisObjPath, apObjPath, isRecoverable, mctpVdmHelper));
            }
            else
            {
                resources.push_back(std::make_unique<ERoTResource>(bus, objPath, i2cBus, i2cAddress, uuid));
            }
        }
    }

    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    event.loop();
}

