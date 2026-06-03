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

#include "config.h"

#include "ap_resource.hpp"
#include "connectx_resource.hpp"
#include "cpld_resource.hpp"
#include "dbusutils.hpp"
#include "erot_resource.hpp"
#include "gpio_resource.hpp"
#include "gpu_resource.hpp"
#include "mctp_endpoint_discovery.hpp"
#include "mctp_vdm_helper.hpp"
#include "mcu_recovery_manager.hpp"
#include "mcu_recovery_mode_manager.hpp"
#include "mcu_resource.hpp"
#include "nvlinkmgmt_nic_resource.hpp"
#include "nvswitch_resource.hpp"
#include "udev_monitor.hpp"
#include "usb_i2c_mapper.hpp"
#include "usb_rcm_resource.hpp"
#include "usbrcm_recovery_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/event.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface =
    "xyz.openbmc_project.Configuration.OCPRecovery";
constexpr auto glacierCrisisObjInterface =
    "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto gpioObjInterface =
    "xyz.openbmc_project.Configuration.GPIORecovery";
constexpr auto mcuObjInterface =
    "xyz.openbmc_project.Configuration.MCURecovery";
constexpr auto connectxObjInterface =
    "xyz.openbmc_project.Configuration.ConnectXRecovery";
constexpr auto nvswitchObjInterface =
    "xyz.openbmc_project.Configuration.NVSwitchRecovery";
constexpr auto nvlinkMgmtNicObjInterface =
    "xyz.openbmc_project.Configuration.NVLinkManagementNICRecovery";
constexpr auto usbRcmForceRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.USBRCMForceRecovery";
constexpr auto usbRcmObjInterface =
    "xyz.openbmc_project.Configuration.USBRCMRecovery";
constexpr auto cpldMonitorObjInterface =
    "xyz.openbmc_project.Configuration.CPLDGPIOMonitor";
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/";
constexpr auto recoveryConfigIntfName =
    "xyz.openbmc_project.Inventory.Item.Recovery_Config";
constexpr auto chassisInterface = "xyz.openbmc_project.State.Chassis";
constexpr auto stateBasePath = "/xyz/openbmc_project/state";

using namespace phosphor::logging;
using namespace nvidia::software::updater;
using namespace mctp_vdm;

std::vector<std::unique_ptr<BaseResource>> resources;

std::unique_ptr<sdbusplus::bus::match_t> entityManagerServiceMatch;
std::unique_ptr<sdbusplus::bus::match_t> chassisPowerStateMatch;
std::unique_ptr<sdbusplus::bus::match_t> chassisDiscoveryRetryMatch;

std::vector<std::unique_ptr<nvidia::recovery::RecoveryModeManagerBase>>
    recoveryModeManagers;

std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
std::shared_ptr<mcu_recovery_manager::MCURecoveryManager> mcuRecoveryManager;
std::shared_ptr<UdevMonitor> udevMonitor;

void checkEntityManagerAvailability();
bool startCentralizedPowerStateWatcher();
void armCentralizedPowerStateWatcherRetry();

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

auto& getEvent()
{
    static auto event = sdeventplus::Event::get_default();
    return event;
}

/**
 * @brief Get the software D-Bus object path
 *
 * @param[in] path The filesystem path
 *
 * @return Software D-Bus object path string
 */
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

std::optional<uint8_t> getUint8(const InterfaceMap& interfaces,
                                const Interface& interface,
                                const Property& property)
{
    try
    {
        // Entity Manager stores small integers as uint64_t
        auto value = std::get<uint64_t>(interfaces.at(interface).at(property));
        if (value > 0xFF)
        {
            lg2::error("Property {NAME} value {VALUE} exceeds uint8_t range",
                       "NAME", property, "VALUE", value);
            return std::nullopt;
        }
        return static_cast<uint8_t>(value);
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return std::nullopt;
    }
}

/**
 * @brief Check if a property exists in a D-Bus interface
 *
 * @param[in] interfaces - Map of D-Bus interfaces and their properties
 * @param[in] interface - The interface to check
 * @param[in] property - The property to check for
 *
 * @return bool - True if the property exists, false otherwise
 */
bool hasProperty(const InterfaceMap& interfaces, const Interface& interface,
                 const Property& property)
{
    try
    {
        return interfaces.at(interface).contains(property);
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to check property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return false;
    }
}

void applyChassisConnectionAndRefresh(const InterfaceMap& interfaces,
                                      const Interface& interface,
                                      BaseResource& resource,
                                      const std::string& initialPowerState)
{
    if (hasProperty(interfaces, interface, "hasChassisPowerSource"))
    {
        resource.setConnectedToChassis(
            getBool(interfaces, interface, "hasChassisPowerSource"));
        if (resource.hasChassisPowerSource())
        {
            resource.setChassisPowerState(initialPowerState);
        }
    }

    // Always trigger the first updateHealth() here.
    // By this point the chassis power state (if any) has been set
    // so the first probe sees correct state
    try
    {
        resource.updateHealth();
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to refresh health for resource {PATH}: {ERROR}",
                   "PATH", resource.getObjectPath(), "ERROR", e.what());
    }
}

/**
 * @brief Retrieves MCU configuration information from Entity Manager D-Bus
 * interface
 *
 * This function queries the Entity Manager D-Bus service to collect
 * configuration information for all MCU (Microcontroller Unit) devices. It
 * looks for objects implementing the MCU Recovery interface and extracts their
 * properties including:
 * - USB port identifier
 * - Reset GPIO name
 * - Recovery GPIO name
 * - Functional Product ID
 *
 * The function processes each MCU configuration object found in the Entity
 * Manager. If a required property is missing for a particular MCU, that MCU
 * is skipped and processing continues with the next one. This allows partial
 * configuration to be loaded even if some MCUs have incomplete configuration.
 *
 * @return std::map<std::string, mcu_recovery_manager::MCUInfo> A map where:
 *         - Key: USB port identifier
 *         - Value: MCUInfo structure containing device configuration
 *         Returns an empty map if:
 *         - No MCU configurations are found
 *         - D-Bus query fails
 */
std::map<std::string, mcu_recovery_manager::MCUInfo> getMCUConfig()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    nvidia::software::updater::ObjectValueTree managedObjects;
    try
    {
        managedObjects = dbusUtil.getManagedObjects(entityManagerService,
                                                    entityManagerObjManager);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to get MCU configuration objects: {ERR}", "ERR",
                   e.what());
        return {};
    }

    std::map<std::string, mcu_recovery_manager::MCUInfo> mcuMap;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(mcuObjInterface))
        {
            std::string interfaceType = "USB";
            if (hasProperty(interfaces, mcuObjInterface, "Interface"))
            {
                interfaceType =
                    getString(interfaces, mcuObjInterface, "Interface");
            }

            if (!hasProperty(interfaces, mcuObjInterface, "Name"))
            {
                continue;
            }
            const auto deviceName =
                getString(interfaces, mcuObjInterface, "Name");

            if (!hasProperty(interfaces, mcuObjInterface, "ResetGpioName"))
            {
                continue;
            }
            const auto resetGpioName =
                getString(interfaces, mcuObjInterface, "ResetGpioName");

            if (!hasProperty(interfaces, mcuObjInterface, "RecoveryGpioName"))
            {
                continue;
            }
            const auto recoveryGpioName =
                getString(interfaces, mcuObjInterface, "RecoveryGpioName");

            mcu_recovery_manager::MCUInfo info;
            info.device = deviceName;
            info.resetGpioName = resetGpioName;
            info.recoveryGpioName = recoveryGpioName;
            info.resetActiveLow = true;
            if (hasProperty(interfaces, mcuObjInterface, "ResetGpioPolarity"))
            {
                const auto pol =
                    getString(interfaces, mcuObjInterface, "ResetGpioPolarity");
                const auto parsed =
                    mcu_recovery_manager::parseActiveLowPolarity(pol);
                if (!parsed)
                {
                    lg2::error(
                        "Invalid ResetGpioPolarity '{POL}' for MCU {DEV}; "
                        "skipping entry. Expected 'ActiveLow' or 'ActiveHigh'.",
                        "POL", pol, "DEV", deviceName);
                    continue;
                }
                info.resetActiveLow = *parsed;
            }
            info.recoveryActiveLow = true;
            if (hasProperty(interfaces, mcuObjInterface,
                            "RecoveryGpioPolarity"))
            {
                const auto pol = getString(interfaces, mcuObjInterface,
                                           "RecoveryGpioPolarity");
                const auto parsed =
                    mcu_recovery_manager::parseActiveLowPolarity(pol);
                if (!parsed)
                {
                    lg2::error(
                        "Invalid RecoveryGpioPolarity '{POL}' for MCU {DEV}; "
                        "skipping entry. Expected 'ActiveLow' or 'ActiveHigh'.",
                        "POL", pol, "DEV", deviceName);
                    continue;
                }
                info.recoveryActiveLow = *parsed;
            }

            if (interfaceType == "I2C")
            {
                if (!hasProperty(interfaces, mcuObjInterface, "I2CBus") ||
                    !hasProperty(interfaces, mcuObjInterface,
                                 "NormalI2CAddress") ||
                    !hasProperty(interfaces, mcuObjInterface,
                                 "RecoveryI2CAddress"))
                {
                    lg2::error("Missing I2C properties for {PATH}", "PATH",
                               emObjectPath);
                    continue;
                }
                info.interfaceType =
                    mcu_recovery_manager::MCUInfo::InterfaceType::I2C;
                info.i2cBus = static_cast<uint8_t>(
                    getUint64(interfaces, mcuObjInterface, "I2CBus"));
                info.normalI2cAddress = static_cast<uint16_t>(
                    getUint64(interfaces, mcuObjInterface, "NormalI2CAddress"));
                info.recoveryI2cAddress = static_cast<uint16_t>(getUint64(
                    interfaces, mcuObjInterface, "RecoveryI2CAddress"));
            }
            else
            {
                if (!hasProperty(interfaces, mcuObjInterface, "USBPort") ||
                    !hasProperty(interfaces, mcuObjInterface, "ProductId"))
                {
                    lg2::error("Missing USB properties for {PATH}", "PATH",
                               emObjectPath);
                    continue;
                }
                info.interfaceType =
                    mcu_recovery_manager::MCUInfo::InterfaceType::USB;
                info.usbPort =
                    getString(interfaces, mcuObjInterface, "USBPort");
                info.functionalPid =
                    getUint64(interfaces, mcuObjInterface, "ProductId");
            }

            mcuMap[info.device] = info;
        }
    }

    return mcuMap;
}

/**
 * @brief Re-evaluate health/state for every registered resource.
 *
 * Used as the recovery-complete callback for the SetRecoveryMode managers: once
 * a force recovery is confirmed, the dependent resources (e.g. the USB RCM
 * SBIOS_FMC/SBIOS_FW objects) must refresh so the firmware inventory reports
 * the in-recovery state immediately, rather than waiting for a udev/MCTP/power
 * event that may not occur on the recovery transition. updateHealth() only
 * updates D-Bus properties (no object creation), so it is safe to call
 * repeatedly.
 */
void refreshAllResourceHealth()
{
    for (auto& resource : resources)
    {
        try
        {
            resource->updateHealth();
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to refresh health for resource {PATH}: {ERR}",
                       "PATH", resource->getObjectPath(), "ERR", e.what());
        }
    }
}

/**
 * @brief Publish the D-Bus recovery object
 *
 * @return None
 */
void publishDBusRecoveryObject()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);
    const auto initialChassisPowerState = dbusUtil.getHostPwrStatus();
    auto& event = getEvent();

    auto mcuMap = getMCUConfig();

    // EntityManager publishes recovery configs incrementally, so this function
    // may run more than once (see checkEntityManagerAvailability). Guard the
    // MCU recovery manager singleton so a later invocation does not rebuild it.
    if (!mcuRecoveryManager && !mcuMap.empty())
    {
        mcuRecoveryManager =
            std::make_shared<mcu_recovery_manager::MCURecoveryManager>();
        auto messageRegistry = std::make_unique<MessageRegistry>(getBus());
        mcuRecoveryManager->initialize(mcuMap, std::move(messageRegistry));
    }

    // Recovery-mode managers (SetRecoveryMode interface) are not tracked in
    // `resources`, so unlike resources they are not dedup'd by the object-path
    // check below. Guard against re-creating a manager on a chassis path that
    // already has one when this function runs again for late-published configs;
    // re-registering an existing sdbusplus object would throw.
    auto recoveryManagerExists = [](const std::string& path) {
        return std::find_if(recoveryModeManagers.begin(),
                            recoveryModeManagers.end(),
                            [&path](const auto& mgr) {
                                return mgr->getObjectPath() == path;
                            }) != recoveryModeManagers.end();
    };

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        const auto objPath =
            getSoftwareDBusObjectPath(std::string(emObjectPath));

        // Check if the object path already exists
        if (std::find_if(resources.begin(), resources.end(),
                         [&objPath](const auto& resource) {
                             return resource->getObjectPath() == objPath;
                         }) != resources.end())
        {
            lg2::info("Object path already registered: {PATH}", "PATH",
                      objPath);
            continue; // Skip registration
        }

        if (interfaces.contains(ocpObjInterface))
        {
            lg2::info("Found OCP recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            uint64_t i2cBus = 0;
            const auto i2cAddress =
                getUint64(interfaces, ocpObjInterface, "I2CAddress");

            const auto eidOpt =
                getUint8(interfaces, ocpObjInterface, "MctpEID");
            if (!eidOpt.has_value())
            {
                lg2::error("Error in Entity Manager configuration: No MctpEID "
                           "found in OCP recovery config Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            const auto chassisName =
                getString(interfaces, ocpObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            // Only create SetRecoveryMode interface if
            // ForceRecoveryChassisObject is specified
            std::string forceRecoveryChassisObjPath;
            if (hasProperty(interfaces, ocpObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                const auto forceRecoveryChassisName = getString(
                    interfaces, ocpObjInterface, "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            std::string inforomObjPath;
            if (hasProperty(interfaces, ocpObjInterface, "InfoRomName"))
            {
                const auto inforomName =
                    getString(interfaces, ocpObjInterface, "InfoRomName");
                inforomObjPath = getSoftwareDBusObjectPath(inforomName);
            }

            if (hasProperty(interfaces, ocpObjInterface, "I2CBus"))
            {
                i2cBus = getUint64(interfaces, ocpObjInterface, "I2CBus");
            }
            else
            {
                if (hasProperty(interfaces, ocpObjInterface, "USBPort"))
                {
                    const auto usbPort =
                        getString(interfaces, ocpObjInterface, "USBPort");
                    const auto busAddr =
                        recovery_tool::usb_i2c::getI2CBusFromUSBPort(usbPort,
                                                                     false);
                    if (busAddr < 0)
                    {
                        lg2::error(
                            "Failed to get I2C bus from USB port {USBPORT}",
                            "USBPORT", usbPort);
                        continue;
                    }

                    i2cBus = busAddr;
                }
                else
                {
                    lg2::error(
                        "No I2CBus or USBPort found in OCP recovery config Object: {PATH}",
                        "PATH", emObjectPath);
                    continue;
                }
            }

            if (hasProperty(interfaces, ocpObjInterface, "SMAEID"))
            {
                const auto smaEidOpt =
                    getUint8(interfaces, ocpObjInterface, "SMAEID");
                if (!smaEidOpt.has_value())
                {
                    lg2::error(
                        "Failed to get SMAEID in OCP recovery config Object: {PATH}",
                        "PATH", emObjectPath);
                    continue;
                }

                const auto smaEID = smaEidOpt.value();

                resources.push_back(std::make_unique<GpuResource>(
                    getBus(), objPath, chassisObjPath,
                    forceRecoveryChassisObjPath, i2cBus, i2cAddress, eid,
                    smaEID, inforomObjPath));
            }
            else
            {
                resources.push_back(std::make_unique<GpuResource>(
                    getBus(), objPath, chassisObjPath,
                    forceRecoveryChassisObjPath, i2cBus, i2cAddress, eid,
                    inforomObjPath));
            }

            applyChassisConnectionAndRefresh(interfaces, ocpObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);
        }
        else if (interfaces.contains(connectxObjInterface))
        {
            lg2::info("Found ConnectX recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            const auto eidOpt =
                getUint8(interfaces, connectxObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No EID found in ConnectX recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, connectxObjInterface, "I2CBus"))
            {
                lg2::error(
                    "No I2CBus found in ConnectX recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cBus =
                getUint64(interfaces, connectxObjInterface, "I2CBus");

            if (!hasProperty(interfaces, connectxObjInterface, "I2CAddress"))
            {
                lg2::error(
                    "No I2CAddress found in ConnectX recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cAddress =
                getUint64(interfaces, connectxObjInterface, "I2CAddress");

            const auto chassisName =
                getString(interfaces, connectxObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            // Only create SetRecoveryMode interface if
            // ForceRecoveryChassisObject is specified
            std::string forceRecoveryChassisObjPath;
            if (hasProperty(interfaces, connectxObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                const auto forceRecoveryChassisName =
                    getString(interfaces, connectxObjInterface,
                              "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            const auto smaEidOpt =
                getUint8(interfaces, connectxObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "Failed to get SMAEID in ConnectX recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto smaEID = smaEidOpt.value();

            std::string resetGpioName;
            if (hasProperty(interfaces, connectxObjInterface, "ResetGPIO"))
            {
                resetGpioName =
                    getString(interfaces, connectxObjInterface, "ResetGPIO");
            }

            std::string flashNotPresentGpioName;
            if (hasProperty(interfaces, connectxObjInterface,
                            "FlashNotPresentGPIO"))
            {
                flashNotPresentGpioName = getString(
                    interfaces, connectxObjInterface, "FlashNotPresentGPIO");
            }

            resources.push_back(std::make_unique<ConnectXResource>(
                getBus(), objPath, chassisObjPath, forceRecoveryChassisObjPath,
                i2cBus, i2cAddress, eid, smaEID, resetGpioName,
                flashNotPresentGpioName));

            applyChassisConnectionAndRefresh(interfaces, connectxObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);
        }
        else if (interfaces.contains(nvlinkMgmtNicObjInterface))
        {
            lg2::info(
                "Found NVLink Management NIC recovery config Object: {PATH}",
                "PATH", emObjectPath);

            const auto eidOpt =
                getUint8(interfaces, nvlinkMgmtNicObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No EID found in NVLink Management NIC recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, nvlinkMgmtNicObjInterface, "I2CBus"))
            {
                lg2::error(
                    "No I2CBus found in NVLink Management NIC recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cBus =
                getUint64(interfaces, nvlinkMgmtNicObjInterface, "I2CBus");

            if (!hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                             "I2CAddress"))
            {
                lg2::error(
                    "No I2CAddress found in NVLink Management NIC recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cAddress =
                getUint64(interfaces, nvlinkMgmtNicObjInterface, "I2CAddress");

            const auto chassisName =
                getString(interfaces, nvlinkMgmtNicObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            std::string forceRecoveryChassisObjPath;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                const auto forceRecoveryChassisName =
                    getString(interfaces, nvlinkMgmtNicObjInterface,
                              "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            const auto smaEidOpt =
                getUint8(interfaces, nvlinkMgmtNicObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "Failed to get SMAEID in NVLink Management NIC recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto smaEID = smaEidOpt.value();

            std::string resetGpioName;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface, "ResetGPIO"))
            {
                resetGpioName = getString(interfaces, nvlinkMgmtNicObjInterface,
                                          "ResetGPIO");
            }

            std::string flashNotPresentGpioName;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                            "FlashNotPresentGPIO"))
            {
                flashNotPresentGpioName =
                    getString(interfaces, nvlinkMgmtNicObjInterface,
                              "FlashNotPresentGPIO");
            }

            resources.push_back(std::make_unique<NVLinkMgmtNicResource>(
                getBus(), objPath, chassisObjPath, forceRecoveryChassisObjPath,
                i2cBus, i2cAddress, eid, smaEID, resetGpioName,
                flashNotPresentGpioName));

            applyChassisConnectionAndRefresh(
                interfaces, nvlinkMgmtNicObjInterface, *resources.back(),
                initialChassisPowerState);
        }
        else if (interfaces.contains(nvswitchObjInterface))
        {
            lg2::info("Found NVSwitch recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            const auto eidOpt =
                getUint8(interfaces, nvswitchObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No EID found in NVSwitch recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, nvswitchObjInterface, "I2CBus"))
            {
                lg2::error(
                    "No I2CBus found in NVSwitch recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cBus =
                getUint64(interfaces, nvswitchObjInterface, "I2CBus");

            if (!hasProperty(interfaces, nvswitchObjInterface, "I2CAddress"))
            {
                lg2::error(
                    "No I2CAddress found in NVSwitch recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cAddress =
                getUint64(interfaces, nvswitchObjInterface, "I2CAddress");

            const auto chassisName =
                getString(interfaces, nvswitchObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            // Only create SetRecoveryMode interface if
            // ForceRecoveryChassisObject is specified
            std::string forceRecoveryChassisObjPath;
            if (hasProperty(interfaces, nvswitchObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                const auto forceRecoveryChassisName =
                    getString(interfaces, nvswitchObjInterface,
                              "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            const auto smaEidOpt =
                getUint8(interfaces, nvswitchObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "Failed to get SMAEID in NVSwitch recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto smaEID = smaEidOpt.value();

            std::string resetGpioName;
            if (hasProperty(interfaces, nvswitchObjInterface, "ResetGPIO"))
            {
                resetGpioName =
                    getString(interfaces, nvswitchObjInterface, "ResetGPIO");
            }

            std::string flashNotPresentGpioName;
            if (hasProperty(interfaces, nvswitchObjInterface,
                            "FlashNotPresentGPIO"))
            {
                flashNotPresentGpioName = getString(
                    interfaces, nvswitchObjInterface, "FlashNotPresentGPIO");
            }

            resources.push_back(std::make_unique<NVSwitchResource>(
                getBus(), objPath, chassisObjPath, forceRecoveryChassisObjPath,
                i2cBus, i2cAddress, eid, smaEID, resetGpioName,
                flashNotPresentGpioName));

            applyChassisConnectionAndRefresh(interfaces, nvswitchObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);
        }
        else if (interfaces.contains(nvlinkMgmtNicObjInterface))
        {
            lg2::info("Found NVLinkMgmtNic recovery config Object: {PATH}",
                      "PATH", emObjectPath);

            const auto eidOpt =
                getUint8(interfaces, nvlinkMgmtNicObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No EID found in NVLinkMgmtNic recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, nvlinkMgmtNicObjInterface, "I2CBus"))
            {
                lg2::error(
                    "No I2CBus found in NVLinkMgmtNic recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cBus =
                getUint64(interfaces, nvlinkMgmtNicObjInterface, "I2CBus");

            if (!hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                             "I2CAddress"))
            {
                lg2::error(
                    "No I2CAddress found in NVLinkMgmtNic recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cAddress =
                getUint64(interfaces, nvlinkMgmtNicObjInterface, "I2CAddress");

            const auto chassisName =
                getString(interfaces, nvlinkMgmtNicObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            std::string forceRecoveryChassisObjPath;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                const auto forceRecoveryChassisName =
                    getString(interfaces, nvlinkMgmtNicObjInterface,
                              "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            const auto smaEidOpt =
                getUint8(interfaces, nvlinkMgmtNicObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "Failed to get SMAEID in NVLinkMgmtNic recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto smaEID = smaEidOpt.value();

            std::string resetGpioName;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface, "ResetGPIO"))
            {
                resetGpioName = getString(interfaces, nvlinkMgmtNicObjInterface,
                                          "ResetGPIO");
            }

            std::string flashNotPresentGpioName;
            if (hasProperty(interfaces, nvlinkMgmtNicObjInterface,
                            "FlashNotPresentGPIO"))
            {
                flashNotPresentGpioName =
                    getString(interfaces, nvlinkMgmtNicObjInterface,
                              "FlashNotPresentGPIO");
            }

            resources.push_back(std::make_unique<NVLinkMgmtNicResource>(
                getBus(), objPath, chassisObjPath, forceRecoveryChassisObjPath,
                i2cBus, i2cAddress, eid, smaEID, resetGpioName,
                flashNotPresentGpioName));

            applyChassisConnectionAndRefresh(
                interfaces, nvlinkMgmtNicObjInterface, *resources.back(),
                initialChassisPowerState);
        }
        else if (interfaces.contains(glacierCrisisObjInterface))
        {
            lg2::info("Found Glacier Crisis recovery config Object: {PATH}",
                      "PATH", emObjectPath);
            const auto isRecoverable =
                getBool(interfaces, glacierCrisisObjInterface, "isRecoverable");

            uint64_t i2cBus = 0, i2cAddress = 0;
            if (isRecoverable)
            {
                i2cBus =
                    getUint64(interfaces, glacierCrisisObjInterface, "I2CBus");
                i2cAddress = getUint64(interfaces, glacierCrisisObjInterface,
                                       "I2CAddress");
            }

            const auto eidOpt =
                getUint8(interfaces, glacierCrisisObjInterface, "MctpEID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No MctpEID found in Glacier Crisis recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();
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
                    const auto apEidOpt = getUint8(
                        interfaces, glacierCrisisObjInterface, "APEID");
                    if (!apEidOpt.has_value())
                    {
                        lg2::error(
                            "No APEID found in Glacier Crisis recovery config Object: {PATH}",
                            "PATH", emObjectPath);
                        continue;
                    }

                    const auto apEid = apEidOpt.value();
                    const auto apName = getString(
                        interfaces, glacierCrisisObjInterface, "APName");
                    const auto apObjPath = getSoftwareDBusObjectPath(apName);
                    resources.push_back(std::make_unique<ERoTResource>(
                        getBus(), objPath, event, i2cBus, i2cAddress, eid,
                        apEid, chassisObjPath, apObjPath, isRecoverable,
                        mctpVdmHelper));
                }
                else
                {
                    resources.push_back(std::make_unique<ERoTResource>(
                        getBus(), objPath, event, eid, chassisObjPath,
                        isRecoverable, mctpVdmHelper));
                }

                applyChassisConnectionAndRefresh(
                    interfaces, glacierCrisisObjInterface, *resources.back(),
                    initialChassisPowerState);
            }
        }
        else if (interfaces.contains(gpioObjInterface))
        {
            const auto eidOpt =
                getUint8(interfaces, gpioObjInterface, "MctpEID");
            if (!eidOpt.has_value())
            {
                lg2::error(
                    "No MctpEID found in GPIO recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();
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
                    getBus(), objPath, event, i2cBus, i2cAddress, eid, gpio,
                    target));
            }
            else
            {
                lg2::info("Found GPIO recovery Object (AP): {PATH}", "PATH",
                          emObjectPath);
                std::string chassisObjPath;

                const auto risingTarget =
                    getString(interfaces, gpioObjInterface, "RisingTarget");
                const auto fallingTarget =
                    getString(interfaces, gpioObjInterface, "FallingTarget");
                const auto polarity =
                    getString(interfaces, gpioObjInterface, "Polarity");

                const auto apBootStatusType =
                    getString(interfaces, gpioObjInterface, "APBootStatusType");
                if (!apBootStatusType.empty())
                {
                    const auto chassisName =
                        getString(interfaces, gpioObjInterface, "ChassisName");
                    chassisObjPath = getChassisObjPath(chassisName);
                }
                resources.push_back(std::make_unique<GPIOResource>(
                    getBus(), objPath, event, eid, gpio, risingTarget,
                    fallingTarget, polarity, chassisObjPath, mctpVdmHelper));
            }

            applyChassisConnectionAndRefresh(interfaces, gpioObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);
        }
        else if (interfaces.contains(cpldMonitorObjInterface))
        {
            lg2::info("Found CPLD monitor config Object: {PATH}", "PATH",
                      emObjectPath);

            const auto smaEidOpt =
                getUint8(interfaces, cpldMonitorObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "No SMAEID found in CPLD monitor config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            if (!hasProperty(interfaces, cpldMonitorObjInterface, "DeviceId"))
            {
                lg2::error(
                    "No DeviceId found in CPLD monitor config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto deviceId =
                getString(interfaces, cpldMonitorObjInterface, "DeviceId");
            if (deviceId.empty())
            {
                lg2::error(
                    "Empty DeviceId in CPLD monitor config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            resources.push_back(std::make_unique<CpldResource>(
                getBus(), objPath, smaEidOpt.value(), deviceId));

            applyChassisConnectionAndRefresh(
                interfaces, cpldMonitorObjInterface, *resources.back(),
                initialChassisPowerState);
        }
        else if (interfaces.contains(mcuObjInterface))
        {
            lg2::info("Found MCU recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            std::string interfaceType = "USB";
            if (hasProperty(interfaces, mcuObjInterface, "Interface"))
            {
                interfaceType =
                    getString(interfaces, mcuObjInterface, "Interface");
            }

            const auto deviceName =
                getString(interfaces, mcuObjInterface, "Name");

            const auto eidOpt =
                getUint8(interfaces, mcuObjInterface, "MctpEID");
            if (!eidOpt.has_value())
            {
                lg2::error("Failed to get EID in MCU recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            resources.push_back(std::make_unique<MCUResource>(
                getBus(), objPath, eid, deviceName, mcuRecoveryManager));

            applyChassisConnectionAndRefresh(interfaces, mcuObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);

            std::string forceRecoveryChassisObjPath;
            std::string forceRecoveryChassisName;
            if (hasProperty(interfaces, mcuObjInterface,
                            "ForceRecoveryChassisObject"))
            {
                forceRecoveryChassisName = getString(
                    interfaces, mcuObjInterface, "ForceRecoveryChassisObject");
                forceRecoveryChassisObjPath =
                    getChassisObjPath(forceRecoveryChassisName);
            }

            // Create MCURecoveryModeManager for D-Bus SetRecoveryMode interface
            if (mcuRecoveryManager && !forceRecoveryChassisObjPath.empty() &&
                !deviceName.empty() &&
                !recoveryManagerExists(forceRecoveryChassisObjPath))
            {
                try
                {
                    lg2::info(
                        "Creating MCURecoveryModeManager: {CHASSIS}, device: {DEV}",
                        "CHASSIS", forceRecoveryChassisName, "DEV", deviceName);
                    recoveryModeManagers.push_back(
                        std::make_unique<
                            nvidia::recovery::MCURecoveryModeManager>(
                            getBus(), forceRecoveryChassisName,
                            forceRecoveryChassisObjPath, mcuRecoveryManager,
                            deviceName));
                    recoveryModeManagers.back()->setRecoveryCompleteCallback(
                        refreshAllResourceHealth);
                }
                catch (const std::exception& e)
                {
                    lg2::error("Failed to create MCURecoveryModeManager: {ERR}",
                               "ERR", e.what());
                }
            }
        }
        else if (interfaces.contains(usbRcmForceRecoveryObjInterface))
        {
            if (!hasProperty(interfaces, usbRcmForceRecoveryObjInterface,
                             "ForceRecoveryChassisObject") ||
                !hasProperty(interfaces, usbRcmForceRecoveryObjInterface,
                             "ConfigType"))
            {
                lg2::error(
                    "USBRCMForceRecovery config missing ForceRecoveryChassisObject or ConfigType: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            auto forceRecoveryChassisName =
                getString(interfaces, usbRcmForceRecoveryObjInterface,
                          "ForceRecoveryChassisObject");
            auto configType = getString(
                interfaces, usbRcmForceRecoveryObjInterface, "ConfigType");
            auto chassisObjPath = getChassisObjPath(forceRecoveryChassisName);

            if (!recoveryManagerExists(chassisObjPath))
            {
                try
                {
                    recoveryModeManagers.push_back(
                        std::make_unique<
                            nvidia::recovery::USBRCMRecoveryManager>(
                            getBus(), forceRecoveryChassisName, chassisObjPath,
                            configType));
                    recoveryModeManagers.back()->setRecoveryCompleteCallback(
                        refreshAllResourceHealth);
                }
                catch (const std::exception& e)
                {
                    lg2::error("Failed to create USBRCMRecoveryManager: {ERR}",
                               "ERR", e.what());
                }
            }
        }
        else if (interfaces.contains(usbRcmObjInterface))
        {
            lg2::info("Found USB RCM recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            if (!hasProperty(interfaces, usbRcmObjInterface, "USBPort"))
            {
                lg2::error("No USBPort found in USB RCM recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto usbPort =
                getString(interfaces, usbRcmObjInterface, "USBPort");

            if (usbPort.empty())
            {
                lg2::error("Empty USBPort in USB RCM recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eidOpt =
                getUint8(interfaces, usbRcmObjInterface, "MctpEID");
            if (!eidOpt.has_value())
            {
                lg2::error("No EID found in USB RCM recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, usbRcmObjInterface,
                             "FMCComponentName"))
            {
                lg2::error(
                    "No FMCComponentName found in USB RCM recovery config "
                    "Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto fmcComponentName =
                getString(interfaces, usbRcmObjInterface, "FMCComponentName");

            if (fmcComponentName.empty())
            {
                lg2::error("Empty FMCComponentName in USB RCM recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            if (!hasProperty(interfaces, usbRcmObjInterface,
                             "FWSComponentName"))
            {
                lg2::error(
                    "No FWSComponentName found in USB RCM recovery config "
                    "Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto fwsComponentName =
                getString(interfaces, usbRcmObjInterface, "FWSComponentName");

            if (fwsComponentName.empty())
            {
                lg2::error("Empty FWSComponentName in USB RCM recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const std::string softwareObjPath =
                "/xyz/openbmc_project/software/";
            const auto primaryObjPath = softwareObjPath + fmcComponentName;
            const auto companionObjPath = softwareObjPath + fwsComponentName;

            // The top-of-loop dedup keys on the EM object path
            // (e.g. FW_CPU_0), which differs from this resource's software path
            // (e.g. SBIOS_FMC_0), so guard explicitly here. Without this, a
            // re-publish for late-added configs would attempt to re-register an
            // existing object and fail with "File exists".
            if (std::find_if(resources.begin(), resources.end(),
                             [&primaryObjPath](const auto& resource) {
                                 return resource->getObjectPath() ==
                                        primaryObjPath;
                             }) != resources.end())
            {
                lg2::info("USB RCM resource already registered: {PATH}", "PATH",
                          primaryObjPath);
                continue;
            }

            resources.push_back(std::make_unique<USBRcmResource>(
                getBus(), primaryObjPath, eid, usbPort, companionObjPath,
                udevMonitor));

            applyChassisConnectionAndRefresh(interfaces, usbRcmObjInterface,
                                             *resources.back(),
                                             initialChassisPowerState);
        }
    }

    if (!startCentralizedPowerStateWatcher())
    {
        armCentralizedPowerStateWatcherRetry();
    }
}

/**
 * @brief Start a single centralized watcher for chassis power state changes.
 *
 * Instead of each resource creating its own D-Bus propertiesChanged matcher,
 * this function creates one global matcher that iterates all resources and
 * updates each resource's cached chassis power state when the chassis power
 * state changes.
 */
bool startCentralizedPowerStateWatcher()
{
    if (chassisPowerStateMatch)
    {
        return true;
    }

    std::string chassisPath;
    try
    {
        auto method = getBus().new_method_call(
            MAPPER_BUSNAME, MAPPER_PATH, MAPPER_INTERFACE, "GetSubTreePaths");
        method.append(stateBasePath);
        method.append(0);
        method.append(std::vector<std::string>({chassisInterface}));

        auto reply = getBus().call(method);
        std::vector<std::string> chassisPaths;
        reply.read(chassisPaths);

        if (!chassisPaths.empty())
        {
            chassisPath = chassisPaths[0];
        }
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to discover chassis path for centralized watcher: {ERROR}",
            "ERROR", e.what());
        return false;
    }

    if (chassisPath.empty())
    {
        lg2::warning(
            "No chassis path found, centralized power state watcher not started");
        return false;
    }

    chassisPowerStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        getBus(), MatchRules::propertiesChanged(chassisPath, chassisInterface),
        [](sdbusplus::message::message& msg) {
            std::string interface;
            std::map<std::string, std::variant<std::string>> properties;

            try
            {
                msg.read(interface, properties);

                auto it = properties.find("CurrentPowerState");
                if (it == properties.end())
                {
                    return;
                }

                const auto& powerState = std::get<std::string>(it->second);
                lg2::info(
                    "Chassis power state changed to {STATE}, updating cached power state for all resources",
                    "STATE", powerState);

                for (auto& resource : resources)
                {
                    if (!resource->hasChassisPowerSource())
                    {
                        continue;
                    }

                    const auto objectPath = resource->getObjectPath();
                    lg2::info(
                        "Updating cached chassis power state for resource {PATH} to {STATE}",
                        "PATH", objectPath, "STATE", powerState);
                    resource->setChassisPowerState(powerState);
                    try
                    {
                        resource->updateHealth();
                    }
                    catch (const std::exception& e)
                    {
                        lg2::error(
                            "Failed to update health for resource at {PATH} after a chassis power state change",
                            "PATH", objectPath);
                    }
                }
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Failed to process centralized power state change: {ERROR}",
                    "ERROR", e.what());
            }
        });

    lg2::info(
        "Started centralized chassis power state watcher at {CHASSIS_PATH}",
        "CHASSIS_PATH", chassisPath);
    return true;
}

void armCentralizedPowerStateWatcherRetry()
{
    if (chassisPowerStateMatch || chassisDiscoveryRetryMatch)
    {
        return;
    }

    chassisDiscoveryRetryMatch = std::make_unique<sdbusplus::bus::match_t>(
        getBus(), MatchRules::interfacesAdded(stateBasePath),
        [](sdbusplus::message::message& msg) {
            try
            {
                sdbusplus::message::object_path objPath;
                std::map<std::string, std::map<std::string, Value>> interfaces;

                msg.read(objPath, interfaces);

                if (!interfaces.contains(chassisInterface))
                {
                    return;
                }

                lg2::info(
                    "Detected chassis interface at {PATH}; retrying centralized "
                    "power state watcher setup",
                    "PATH", std::string(objPath));

                if (startCentralizedPowerStateWatcher())
                {
                    chassisDiscoveryRetryMatch.reset();
                }
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Failed to process chassis discovery retry signal: {ERROR}",
                    "ERROR", e.what());
            }
        });

    lg2::info(
        "Armed centralized power state watcher retry on interfacesAdded for {PATH}",
        "PATH", stateBasePath);

    // Close a race where the chassis object appears between initial discovery
    // failure and retry matcher registration.
    if (startCentralizedPowerStateWatcher())
    {
        chassisDiscoveryRetryMatch.reset();
    }
}

/**
 * @brief Get recovery configurations from the D-Bus
 *
 * @return True when configurations are available on D-Bus,
 *          and false otherwise
 */
bool checkForRecoveryConfigEMObjects()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(recoveryConfigIntfName))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks the availability of the EntityManager service and triggers
 *        the publishing of the D-Bus recovery objects.
 *
 * EntityManager publishes recovery configuration objects incrementally, and the
 * different recovery types (Glacier, MCU, USBRCM, ...) all share the generic
 * 'recoveryConfigIntfName' marker interface. A single snapshot taken when the
 * first config appears can therefore miss configs (e.g. USBRCM) that are
 * published a moment later, which would leave their D-Bus objects (including
 * the USBRCM 'com.nvidia.SetRecoveryMode' interface on the chassis)
 * unregistered.
 *
 * To be robust against this startup race, the interfacesAdded match is kept
 * armed for the lifetime of the service and 'publishDBusRecoveryObject()' is
 * (re)invoked every time a recovery config interface is added. That function is
 * idempotent: existing resources and recovery-mode managers are not recreated,
 * so late-published configs are picked up while already-created ones are left
 * untouched. Any configs already present are published immediately as well.
 */
void checkEntityManagerAvailability()
{
    if (!entityManagerServiceMatch)
    {
        entityManagerServiceMatch = std::make_unique<sdbusplus::bus::match_t>(
            getBus(), MatchRules::interfacesAdded(entityManagerObjManager),
            []([[maybe_unused]] sdbusplus::message::message& msg) {
                sdbusplus::message::object_path objPath;
                std::map<std::string, std::map<std::string, Value>> interfaces;
                msg.read(objPath, interfaces);

                for (const auto& [interfaceName, properties] : interfaces)
                {
                    if (interfaceName == recoveryConfigIntfName)
                    {
                        // Idempotent: only newly-published configs are acted
                        // on.
                        publishDBusRecoveryObject();
                        break;
                    }
                }
            });
    }

    if (checkForRecoveryConfigEMObjects())
    {
        publishDBusRecoveryObject();
    }
}

/**
 * @brief Main function to initialize and start the service
 *
 * @return int Exit status
 */
int main()
{
    auto& bus = getBus();
    bus.request_name(fwStatusService);

    sdbusplus::server::manager_t mgr{bus, fwStatusObjManager};

    auto& event = getEvent();
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);

    try
    {
        udevMonitor = std::make_shared<UdevMonitor>(event);
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "Failed to initialize UdevMonitor, USB monitoring disabled: {ERR}",
            "ERR", e.what());
    }

    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr instanceIdMgr;

    requester::Handler reqHandler(event, instanceIdMgr, sockManager);
    mctp_socket::Handler sockHandler(event, reqHandler, sockManager);

    mctpVdmHelper = std::make_shared<MCTPVdmHelper>(getBus(), reqHandler,
                                                    sockHandler, instanceIdMgr);

    std::unique_ptr<mctp_vdm::MctpDiscovery> mctpDiscoveryHandler =
        std::make_unique<mctp_vdm::MctpDiscovery>(
            getBus(), sockHandler,
            std::initializer_list<mctp_vdm::MctpDiscoveryHandlerIntf*>{
                mctpVdmHelper.get()});

    checkEntityManagerAvailability();

    event.loop();
}
