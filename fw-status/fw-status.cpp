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
#include "mcu_resource.hpp"
#include "usb_i2c_mapper.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/event.hpp>

#include <filesystem>

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
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/";
constexpr auto configurableStateManagerService =
    "xyz.openbmc_project.State.ConfigurableStateManager";
constexpr auto configurableStateManagerPath =
    "/xyz/openbmc_project/state/configurableStateManager";
constexpr auto configurableStateManagerMctpPath =
    "/xyz/openbmc_project/state/configurableStateManager/MCTP";
constexpr auto csmFeatureReadyStateIntfName =
    "xyz.openbmc_project.State.FeatureReady";
constexpr auto csmFeatureReadyStateEnabled =
    "xyz.openbmc_project.State.FeatureReady.States.Enabled";
constexpr auto recoveryConfigIntfName =
    "xyz.openbmc_project.Inventory.Item.Recovery_Config";

using namespace phosphor::logging;
using namespace nvidia::software::updater;
using namespace mctp_vdm;

std::vector<std::unique_ptr<BaseResource>> resources;

// Define the maps to hold unique matches
std::unique_ptr<sdbusplus::bus::match_t> csmServiceMatch;
std::unique_ptr<sdbusplus::bus::match_t> csmServiceStateMatch;
std::unique_ptr<sdbusplus::bus::match_t> entityManagerServiceMatch;

#ifdef MCTP_IN_KERNEL
using TRequest = mctp_vdm::requester::InKernelRequest;
using TSocketHandler = mctp_socket::InKernelHandler;
#else
using TRequest = mctp_vdm::requester::DaemonRequest;
using TSocketHandler = mctp_socket::DaemonHandler;
#endif

template class mctp_vdm::MctpDiscovery<TRequest>;
template class mctp_socket::Handler<TRequest>;

std::shared_ptr<MCTPVdmHelper<TRequest>> mctpVdmHelper;
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
 * @brief Retrieves the UUID of an MCTP endpoint given its EID (Endpoint ID)
 *
 * This function queries the D-Bus to find the MCTP endpoint with the specified
 * EID and returns its associated UUID. It searches through all MCTP services
 * and their managed objects to find a matching endpoint.
 *
 * @param[in] eid - The MCTP Endpoint ID to look up
 *
 * @return std::string - The UUID of the MCTP endpoint if found, empty string if
 * not found
 */
std::string getMctpUUID(uint32_t eid)
{
    nvidia::software::updater::GetSubTreeResponse getSubTreeResponse{};
    const nvidia::software::updater::Interfaces ifaceList{mctpEndpointIntfName};
    try
    {
        auto method = getBus().new_method_call(mapperService, mapperPath,
                                               mapperInterface, "GetSubTree");
        method.append("/xyz/openbmc_project/mctp", 0, ifaceList);
        auto reply = getBus().call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "D-Bus error calling Subtrees method on ObjectMapper: {ERROR}",
            "ERROR", e.what());
    }

    for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
    {
        for (const auto& [service, interfaces] : mapperServiceMap)
        {
            auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
            const auto objects = dbusUtil.getManagedObjects(
                service.c_str(), "/xyz/openbmc_project/mctp");

            for (const auto& [objectPath, interfaces] : objects)
            {
                if (!interfaces.contains(uuidIntfName) ||
                    !interfaces.contains(mctpEndpointIntfName))
                {
                    continue;
                }

                const auto& mctpEID = std::get<uint32_t>(
                    interfaces.at(mctpEndpointIntfName).at("EID"));

                if (eid == mctpEID)
                {
                    const auto& mctpUUID = std::get<std::string>(
                        interfaces.at(uuidIntfName).at("UUID"));
                    return mctpUUID;
                }
            }
        }
    }
    return {};
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

            std::string uuid;

            if (hasProperty(interfaces, ocpObjInterface, "MctpUUID"))
            {
                uuid = getString(interfaces, ocpObjInterface, "MctpUUID");
            }
            else
            {
                lg2::info(
                    "MctpUUID not found in OCP recovery config Object: {PATH}",
                    "PATH", emObjectPath);
                if (hasProperty(interfaces, ocpObjInterface, "MctpEID"))
                {
                    const auto eid =
                        getUint64(interfaces, ocpObjInterface, "MctpEID");
                    uuid = getMctpUUID(eid);
                    if (uuid.empty())
                    {
                        lg2::error(
                            "Failed to get MCTP UUID for EID {EID} in OCP recovery config "
                            "Object: {PATH}",
                            "EID", eid, "PATH", emObjectPath);
                        continue;
                    }
                }
                else
                {
                    lg2::error(
                        "Error in Entity Manager configuration: No MctpUUID or MctpEID "
                        "found in OCP recovery config Object: {PATH}",
                        "PATH", emObjectPath);
                    continue;
                }
            }

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
                const auto smaEID =
                    getUint64(interfaces, ocpObjInterface, "SMAEID");

                resources.push_back(std::make_unique<GpuResource>(
                    getBus(), objPath, chassisObjPath, i2cBus, i2cAddress, uuid,
                    smaEID));
            }
            else
            {
                resources.push_back(std::make_unique<GpuResource>(
                    getBus(), objPath, chassisObjPath, i2cBus, i2cAddress,
                    uuid));
            }
        }
        else if (interfaces.contains(cx8ObjInterface))
        {
            lg2::info("Found CX8 recovery config Object: {PATH}", "PATH",
                      emObjectPath);

            const auto eid = getUint64(interfaces, cx8ObjInterface, "EID");
            const auto uuid = getMctpUUID(eid);
            if (uuid.empty())
            {
                lg2::error(
                    "Failed to get MCTP UUID for EID {EID} in CX8 recovery config "
                    "Object: {PATH}",
                    "EID", eid, "PATH", emObjectPath);
                continue;
            }

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

            const auto smaEID =
                getUint64(interfaces, cx8ObjInterface, "SMAEID");

            resources.push_back(std::make_unique<Cx8Resource>(
                getBus(), objPath, chassisObjPath, i2cBus, i2cAddress, uuid,
                smaEID));
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
                    resources.push_back(
                        std::make_unique<ERoTResource<TRequest>>(
                            getBus(), objPath, event, i2cBus, i2cAddress, uuid,
                            apEid, chassisObjPath, apObjPath, isRecoverable,
                            mctpVdmHelper));
                }
                else
                {
                    resources.push_back(
                        std::make_unique<ERoTResource<TRequest>>(
                            getBus(), objPath, event, uuid, chassisObjPath,
                            isRecoverable, mctpVdmHelper));
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
                resources.push_back(std::make_unique<GPIOResource<TRequest>>(
                    getBus(), objPath, event, i2cBus, i2cAddress, uuid, gpio,
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
                resources.push_back(std::make_unique<GPIOResource<TRequest>>(
                    getBus(), objPath, event, uuid, gpio, risingTarget,
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

            if (!hasProperty(interfaces, mcuObjInterface, "EID"))
            {
                lg2::error("Failed to get EID in SMA recovery config "
                           "Object: {PATH}",
                           "PATH", emObjectPath);
                continue;
            }

            const auto eid = getUint64(interfaces, mcuObjInterface, "EID");
            const auto uuid = getMctpUUID(eid);
            if (uuid.empty())
            {
                lg2::error(
                    "Failed to get MCTP UUID for EID {EID} in SMA recovery config "
                    "Object: {PATH}",
                    "EID", eid, "PATH", emObjectPath);
                continue;
            }

            resources.push_back(std::make_unique<MCUResource>(
                getBus(), objPath, uuid, usbPort, mcuRecoveryManager));
        }
    }
}

/**
 * @brief Callback for CSM service state change message
 *
 * @param[in] msg The D-Bus message
 *
 * @return None
 */
void onMCTPServiceStateChangeMsg(sdbusplus::message::message& msg)
{
    std::string iface;
    std::map<std::string, std::variant<std::string, bool, uint8_t>>
        changedProperties;
    std::vector<std::string> invalidatedProperties;

    msg.read(iface, changedProperties, invalidatedProperties);

    if (iface == csmFeatureReadyStateIntfName &&
        changedProperties.find("State") != changedProperties.end())
    {
        checkEntityManagerAvailability();
    }
}

/**
 * @brief Add a match if it doesn't already exist
 *
 * @param[in] objectPath The D-Bus object path
 * @param[in] interfaceName The D-Bus interface name
 * @param[in] callback The callback function to be called on match
 *
 * @return None
 */
void addServiceStateMatch(
    const std::string& objectPath, const std::string& interfaceName,
    std::function<void(sdbusplus::message::message&)> callback)
{
    try
    {
        csmServiceStateMatch = std::make_unique<sdbusplus::bus::match_t>(
            getBus(), MatchRules::propertiesChanged(objectPath, interfaceName),
            callback);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "D-Bus error while creating propertiesChanged event for {OBJPATH}: {ERROR} ",
            "OBJPATH", objectPath, "ERROR", e.what());
    }
}

/**
 * @brief Try to publish the D-Bus recovery object
 *
 * @param[in] createInterfaceAddedEvent If true, sets up a match to listen for
 *                                      `InterfacesAdded` events.
 * @return None
 */
void tryPublishDBusRecoveryObject(bool createInterfaceAddedEvent = true)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    bool servicesEnabled = false;

    if (createInterfaceAddedEvent)
    {
        csmServiceMatch = std::make_unique<sdbusplus::bus::match_t>(
            getBus(), MatchRules::interfacesAdded(configurableStateManagerPath),
            [](sdbusplus::message::message& msg) {
                sdbusplus::message::object_path objPath;
                std::map<std::string, std::map<std::string, Value>> interfaces;
                msg.read(objPath, interfaces);

                if (objPath.str == configurableStateManagerMctpPath)
                {
                    tryPublishDBusRecoveryObject(false);
                }
            });
    }

    try
    {
        auto state = dbusUtil.getProperty<std::string>(
            configurableStateManagerService, configurableStateManagerMctpPath,
            csmFeatureReadyStateIntfName, "State");

        servicesEnabled = (state == csmFeatureReadyStateEnabled);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "D-Bus error while fetching state property for {SERVICE}, {OBJPATH}: {ERROR} ",
            "SERVICE", configurableStateManagerService, "OBJPATH",
            configurableStateManagerMctpPath, "ERROR", e.what());
        return;
    }

    csmServiceMatch.reset();

    if (servicesEnabled)
    {
        checkEntityManagerAvailability();
    }
    else
    {
        addServiceStateMatch(configurableStateManagerMctpPath,
                             csmFeatureReadyStateIntfName,
                             onMCTPServiceStateChangeMsg);
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
 *
 * @param mctpVdmHelper A shared pointer to an 'MCTPVdmHelper' object, which is
 *        used in the process of publishing the D-Bus recovery object.
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

    requester::Handler<TRequest> reqHandler(event, instanceIdMgr, sockManager);
    TSocketHandler sockHandler(event, reqHandler, sockManager);

    mctpVdmHelper = std::make_shared<MCTPVdmHelper<TRequest>>(
        getBus(), reqHandler, sockHandler, instanceIdMgr);

    std::unique_ptr<mctp_vdm::MctpDiscovery<TRequest>> mctpDiscoveryHandler =
        std::make_unique<mctp_vdm::MctpDiscovery<TRequest>>(
            getBus(), sockHandler,
            std::initializer_list<mctp_vdm::MctpDiscoveryHandlerIntf*>{
                mctpVdmHelper.get()});

    tryPublishDBusRecoveryObject();

    event.loop();
}
