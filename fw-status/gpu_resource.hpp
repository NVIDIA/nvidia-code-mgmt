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

#include "mctp_discovery_resource.hpp"
#include "recovery_commands.hpp"
#include "recoverytool_utils.hpp"

#include <format>
#include <memory>

/**@class GpuResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the OCP Recovery Protocol
 *
 *
 */
class GpuResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the GpuResource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param chassisObjPath - Path of D-Bus object for boot status
     * @param forceRecoveryChassisObjPath - Path for SetRecoveryMode interface
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param eid - MCTP Endpoint ID of the Resource
     * @param inforomObjPath - Path of companion InfoROM D-Bus object (optional)
     *
     */
    GpuResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& chassisObjPath,
                const std::string& forceRecoveryChassisObjPath,
                const uint64_t i2cBus, const uint64_t i2cAddress, uint8_t eid,
                const std::string& inforomObjPath = "") :
        MCTPDiscoveryResource(bus, objPath, eid)
    {
        if (!inforomObjPath.empty())
        {
            inforomResource =
                std::make_unique<BaseResource>(bus, inforomObjPath);
        }

        ocpRecoveryCommands = std::make_unique<
            recovery_tool::recovery_commands::OCPRecoveryCommands>(
            i2cBus, i2cAddress, false, false);

        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::OCPDeviceStatus);

        createRecoveryModeInterface(bus, forceRecoveryChassisObjPath);

        updateHealth();
    }

    /**@brief Constructor for the GpuResource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param chassisObjPath - Path of D-Bus object for boot status
     * @param forceRecoveryChassisObjPath - Path for SetRecoveryMode interface
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param eid - MCTP Endpoint ID of the Resource
     * @param smaEid - EID of the SMA
     * @param inforomObjPath - Path of companion InfoROM D-Bus object (optional)
     */
    GpuResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& chassisObjPath,
                const std::string& forceRecoveryChassisObjPath,
                const uint64_t i2cBus, const uint64_t i2cAddress, uint8_t eid,
                uint8_t smaEid, const std::string& inforomObjPath) :
        MCTPDiscoveryResource(bus, objPath, eid), smaEid(smaEid)
    {
        if (!inforomObjPath.empty())
        {
            inforomResource =
                std::make_unique<BaseResource>(bus, inforomObjPath);
        }

        ocpRecoveryCommands = std::make_unique<
            recovery_tool::recovery_commands::OCPRecoveryCommands>(
            i2cBus, i2cAddress, false, false);

        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::OCPDeviceStatus);

        createRecoveryModeInterface(bus, forceRecoveryChassisObjPath);

        updateHealth();

        monitorSMAEndpoint();
    }

  private:
    std::unique_ptr<recovery_tool::recovery_commands::OCPRecoveryCommands>
        ocpRecoveryCommands;
    std::unique_ptr<BootStatus> bootStatus;
    std::unique_ptr<SetRecoveryModeInterface> recoveryModeInterface;
    std::unique_ptr<BaseResource> inforomResource;
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointAddedMatch;
    std::string smaMctpObjectPath;
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointRemovedMatch;
    uint8_t smaEid{0};

    /**@brief Creates the SetRecoveryMode D-Bus interface on the chassis path
     *
     * @param bus - SystemD bus to publish the object
     * @param forceRecoveryChassisObjPath - Chassis D-Bus object path for the
     * SetRecoveryMode interface. If empty, the interface is not created.
     */
    void createRecoveryModeInterface(
        sdbusplus::bus::bus& bus,
        const std::string& forceRecoveryChassisObjPath)
    {
        if (forceRecoveryChassisObjPath.empty())
        {
            return;
        }

        lg2::info("Creating SetRecoveryMode interface on {PATH}", "PATH",
                  forceRecoveryChassisObjPath);

        recoveryModeInterface = std::make_unique<SetRecoveryModeInterface>(
            bus, forceRecoveryChassisObjPath,
            [this, forceRecoveryChassisObjPath]() {
                lg2::info("Performing OCP force recovery for {PATH}", "PATH",
                          forceRecoveryChassisObjPath);

                auto [success, error] =
                    ocpRecoveryCommands->setForceRecoveryMode();
                if (!success)
                {
                    lg2::error("OCP force recovery failed for {PATH}: {ERR}",
                               "PATH", forceRecoveryChassisObjPath, "ERR",
                               error);
                    throw std::runtime_error(error);
                }

                lg2::info("OCP force recovery successful for {PATH}", "PATH",
                          forceRecoveryChassisObjPath);
            });
    }

    /* @brief Override function for updating Health and Status of D-Bus object
     * based on Device Status and MCTP enumeration
     * Uses OCP Recovery Protocol to fetch device status
     *
     * @return void
     */
    void updateHealth() override
    {
        HealthServer::HealthType healthValue;
        OperationalStatusServer::StateType stateValue;

        if (isChassisPoweredOff())
        {
            healthValue = HealthServer::HealthType::Warning;
            stateValue = OperationalStatusServer::StateType::UnavailableOffline;
            health(healthValue);
            state(stateValue);

            if (inforomResource)
            {
                inforomResource->health(healthValue);
                inforomResource->state(stateValue);
            }
            return;
        }

        const auto& [ret, output, errorMsg] =
            ocpRecoveryCommands->getDeviceStatusCommand();

        if (!ret)
        {
            lg2::error(
                "Device associated with {PATH} is not accessible: {ERROR}",
                "PATH", path.c_str(), "ERROR", errorMsg);

            bootStatus->bootStatus({0});
            healthValue = HealthServer::HealthType::Critical;
            if (MCTPDiscoveryResource::wasDeviceEnumeratedBefore())
            {
                stateValue =
                    OperationalStatusServer::StateType::UnavailableOffline;
            }
            else
            {
                stateValue = OperationalStatusServer::StateType::Absent;
            }

            health(healthValue);
            state(stateValue);

            if (inforomResource)
            {
                inforomResource->health(healthValue);
                inforomResource->state(stateValue);
            }
            return;
        }

        const auto status = static_cast<recovery_tool::DeviceStatus>(output[1]);
        bootStatus->bootStatus(
            std::vector<uint8_t>(output.begin() + 1, output.end()));

        if (MCTPDiscoveryResource::isDeviceEnumerated() and
            status == recovery_tool::DeviceStatus::DeviceHealthy)
        {
            lg2::info("MCTP EID for {PATH} is enumerated", "PATH",
                      path.c_str());
            healthValue = HealthServer::HealthType::OK;
            stateValue = OperationalStatusServer::StateType::Enabled;
        }
        else if (status != recovery_tool::DeviceStatus::DeviceHealthy and
                 status != recovery_tool::DeviceStatus::RecoveryImgRunning)
        {
            lg2::info("Device associated with {PATH} is in recovery", "PATH",
                      path.c_str());
            healthValue = HealthServer::HealthType::Critical;
            stateValue = OperationalStatusServer::StateType::StandbyOffline;
        }
        else
        {
            lg2::warning("Device associated with {PATH} is healthy but MCTP "
                         "connectivity is not available",
                         "PATH", path.c_str());
            healthValue = HealthServer::HealthType::Critical;
            stateValue = OperationalStatusServer::StateType::Degraded;
        }

        health(healthValue);
        state(stateValue);

        if (inforomResource)
        {
            inforomResource->health(healthValue);
            inforomResource->state(stateValue);
        }
    }

    /**@brief Fetches the MCTP object path for the SMA EID
     *
     * @return string - MCTP object path, empty if not found
     *
     */
    std::string getSMAMCTPObjectPath()
    {
        auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
        const auto objects =
            dbusUtil.getManagedObjects(mctpService, mctpObjMgrPath.data());

        for (const auto& [objectPath, interfaces] : objects)
        {
            if (!interfaces.contains(mctpEndpointIntfName))
            {
                continue;
            }

            const auto* mctpEID = std::get_if<uint8_t>(
                &interfaces.at(mctpEndpointIntfName).at("EID"));

            if (mctpEID && (*mctpEID == smaEid))
            {
                return objectPath;
            }
        }
        return {};
    }

    /**@brief Monitor SMA MCTP endpoint for add/remove events
     */
    void monitorSMAEndpoint()
    {
        smaMctpObjectPath = getSMAMCTPObjectPath();

        if (!smaEndpointAddedMatch)
        {
            smaEndpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                MatchRules::interfacesAdded(mctpObjMgrPath.data()) +
                    MatchRules::sender(mctpService),
                [this](sdbusplus::message::message& msg) {
                    try
                    {
                        sdbusplus::message::object_path addedPath;
                        nvidia::software::updater::InterfaceMap interfaces;
                        msg.read(addedPath, interfaces);

                        if (!interfaces.contains(mctpEndpointIntfName))
                        {
                            return;
                        }

                        const auto* mctpEID = std::get_if<uint8_t>(
                            &interfaces.at(mctpEndpointIntfName).at("EID"));
                        if (mctpEID && (*mctpEID == smaEid))
                        {
                            smaMctpObjectPath = addedPath.str;
                            updateHealth();
                        }
                    }
                    catch (const std::exception& e)
                    {
                        lg2::error(
                            "Failed to process SMA MCTP interfacesAdded signal: {ERROR}",
                            "ERROR", e);
                    }
                });
        }

        if (!smaEndpointRemovedMatch)
        {
            smaEndpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                MatchRules::interfacesRemoved(mctpObjMgrPath.data()) +
                    MatchRules::sender(mctpService),
                [this](sdbusplus::message::message& msg) {
                    try
                    {
                        sdbusplus::message::object_path removedPath;
                        msg.read(removedPath);

                        if (removedPath.str == smaMctpObjectPath)
                        {
                            smaMctpObjectPath.clear();
                            updateHealth();
                        }
                    }
                    catch (const std::exception& e)
                    {
                        lg2::error(
                            "Failed to process SMA MCTP interfacesRemoved signal: {ERROR}",
                            "ERROR", e);
                    }
                });
        }
    }
};
