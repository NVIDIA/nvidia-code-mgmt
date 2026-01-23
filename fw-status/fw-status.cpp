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
#include "cx8_resource.hpp"
#include "dbusutils.hpp"
#include "erot_resource.hpp"
#include "gpio_resource.hpp"
#include "gpu_resource.hpp"
#include "mctp_endpoint_discovery.hpp"
#include "mctp_vdm_helper.hpp"
#include "mcu_recovery_manager.hpp"
#include "mcu_recovery_mode_manager.hpp"
#include "mcu_resource.hpp"
#include "usb_i2c_mapper.hpp"
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
constexpr auto cx8ObjInterface =
    "xyz.openbmc_project.Configuration.CX8Recovery";
constexpr auto usbRcmForceRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.USBRCMForceRecovery";
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/";
constexpr auto recoveryConfigIntfName =
    "xyz.openbmc_project.Inventory.Item.Recovery_Config";

using namespace phosphor::logging;
using namespace nvidia::software::updater;
using namespace mctp_vdm;

std::vector<std::unique_ptr<BaseResource>> resources;

std::unique_ptr<sdbusplus::bus::match_t> entityManagerServiceMatch;

std::vector<std::unique_ptr<nvidia::recovery::RecoveryModeManagerBase>>
    recoveryModeManagers;

std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
std::shared_ptr<mcu_recovery_manager::MCURecoveryManager> mcuRecoveryManager;

void checkEntityManagerAvailability();

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
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);

    std::map<std::string, mcu_recovery_manager::MCUInfo> mcuMap;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(mcuObjInterface))
        {
            if (!hasProperty(interfaces, mcuObjInterface, "USBPort"))
            {
                continue;
            }

            const auto usbPort =
                getString(interfaces, mcuObjInterface, "USBPort");

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

            if (!hasProperty(interfaces, mcuObjInterface, "ProductId"))
            {
                continue;
            }
            auto functionalPid =
                getUint64(interfaces, mcuObjInterface, "ProductId");

            mcu_recovery_manager::MCUInfo info;
            info.usbPort = usbPort;
            info.resetGpioName = resetGpioName;
            info.recoveryGpioName = recoveryGpioName;
            info.functionalPid = functionalPid;
            mcuMap[info.usbPort] = info;
        }
    }

    return mcuMap;
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
    auto& event = getEvent();

    auto mcuMap = getMCUConfig();

    if (!mcuMap.empty())
    {
        mcuRecoveryManager =
            std::make_shared<mcu_recovery_manager::MCURecoveryManager>();
        auto messageRegistry = std::make_unique<MessageRegistry>(getBus());
        mcuRecoveryManager->initialize(mcuMap, std::move(messageRegistry));
    }

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
                    getBus(), objPath, chassisObjPath, i2cBus, i2cAddress, eid,
                    smaEID));
            }
            else
            {
                resources.push_back(std::make_unique<GpuResource>(
                    getBus(), objPath, chassisObjPath, i2cBus, i2cAddress,
                    eid));
            }
        }
        else if (interfaces.contains(cx8ObjInterface))
        {
            lg2::info("Found CX8 recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            const auto eidOpt = getUint8(interfaces, cx8ObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error("No EID found in CX8 recovery config Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, cx8ObjInterface, "I2CBus"))
            {
                lg2::error(
                    "No I2CBus found in CX8 recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cBus =
                getUint64(interfaces, cx8ObjInterface, "I2CBus");

            if (!hasProperty(interfaces, cx8ObjInterface, "I2CAddress"))
            {
                lg2::error(
                    "No I2CAddress found in CX8 recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto i2cAddress =
                getUint64(interfaces, cx8ObjInterface, "I2CAddress");

            const auto chassisName =
                getString(interfaces, cx8ObjInterface, "ChassisName");
            const auto chassisObjPath = getChassisObjPath(chassisName);

            const auto smaEidOpt =
                getUint8(interfaces, cx8ObjInterface, "SMAEID");
            if (!smaEidOpt.has_value())
            {
                lg2::error(
                    "Failed to get SMAEID in CX8 recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            const auto smaEID = smaEidOpt.value();

            resources.push_back(
                std::make_unique<Cx8Resource>(getBus(), objPath, chassisObjPath,
                                              i2cBus, i2cAddress, eid, smaEID));
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
        }
        else if (interfaces.contains(mcuObjInterface))
        {
            lg2::info("Found MCU recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            if (!hasProperty(interfaces, mcuObjInterface, "USBPort"))
            {
                lg2::error("Failed to get USB Port in MCU recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto usbPort =
                getString(interfaces, mcuObjInterface, "USBPort");

            const auto eidOpt = getUint8(interfaces, mcuObjInterface, "EID");
            if (!eidOpt.has_value())
            {
                lg2::error("Failed to get EID in MCU recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = eidOpt.value();

            if (!hasProperty(interfaces, mcuObjInterface, "ChassisName"))
            {
                lg2::error("Failed to get Chassis Name in MCU recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }
            const auto chassisName =
                getString(interfaces, mcuObjInterface, "ChassisName");

            resources.push_back(std::make_unique<MCUResource>(
                getBus(), objPath, eid, usbPort, mcuRecoveryManager));

            // Create MCURecoveryModeManager for D-Bus SetRecoveryMode interface
            if (mcuRecoveryManager && !chassisName.empty() && !usbPort.empty())
            {
                try
                {
                    lg2::info(
                        "Creating MCURecoveryModeManager: {CHASSIS}, USB port: {PORT}",
                        "CHASSIS", chassisName, "PORT", usbPort);
                    recoveryModeManagers.push_back(
                        std::make_unique<
                            nvidia::recovery::MCURecoveryModeManager>(
                            getBus(), chassisName,
                            getChassisObjPath(chassisName), mcuRecoveryManager,
                            usbPort));
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
                             "ChassisName") ||
                !hasProperty(interfaces, usbRcmForceRecoveryObjInterface,
                             "ConfigType"))
            {
                lg2::error(
                    "USBRCMForceRecovery config missing ChassisName or ConfigType: {PATH}",
                    "PATH", emObjectPath);
                continue;
            }

            auto chassisName = getString(
                interfaces, usbRcmForceRecoveryObjInterface, "ChassisName");
            auto configType = getString(
                interfaces, usbRcmForceRecoveryObjInterface, "ConfigType");
            auto chassisObjPath = getChassisObjPath(chassisName);

            try
            {
                recoveryModeManagers.push_back(
                    std::make_unique<nvidia::recovery::USBRCMRecoveryManager>(
                        getBus(), chassisName, chassisObjPath, configType));
            }
            catch (const std::exception& e)
            {
                lg2::error("Failed to create USBRCMRecoveryManager: {ERR}",
                           "ERR", e.what());
            }
        }
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
 *        the publishing of the D-Bus recovery object based on the presence
 *        of recovery configurations.
 *
 * This function first attempts to retrieve the recovery configurations using
 * the 'checkForRecoveryConfigEMObjects()' function. If no configurations are
 * found (i.e., the vector is empty), it sets up a D-Bus match rule to listen
 * for the addition of specific interfaces on the EntityManager service. When
 * such an interface is added, the function will publish the D-Bus recovery
 * object via the 'publishDBusRecoveryObject()' function.
 *
 * If the recovery configurations are already present, the function immediately
 * publishes the DBus recovery object without waiting for any interface to be
 * added.
 */
void checkEntityManagerAvailability()
{
    if (!checkForRecoveryConfigEMObjects())
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
                        publishDBusRecoveryObject();
                        entityManagerServiceMatch.reset();
                        break;
                    }
                }
            });
    }
    else
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
