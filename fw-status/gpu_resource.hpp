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
     * @param chassisObjPath - Path of D-Bus object to publish
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param uuid - UUID of the Resource
     *
     */
    GpuResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& chassisObjPath, const uint64_t i2cBus,
                const uint64_t i2cAddress, const std::string& uuid) :
        MCTPDiscoveryResource(bus, objPath, uuid)
    {
        ocpRecoveryCommands = std::make_unique<
            recovery_tool::recovery_commands::OCPRecoveryCommands>(
            i2cBus, i2cAddress, false, false);

        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::OCPDeviceStatus);

        updateHealth();
    }

    /**@brief Constructor for the GpuResource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param chassisObjPath - Path of D-Bus object to publish
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param uuid - UUID of the Resource
     * @param smaEid - EID of the SMA
     */
    GpuResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& chassisObjPath, const uint64_t i2cBus,
                const uint64_t i2cAddress, const std::string& uuid,
                const uint32_t smaEid) :
        MCTPDiscoveryResource(bus, objPath, uuid), smaEid(smaEid)
    {
        ocpRecoveryCommands = std::make_unique<
            recovery_tool::recovery_commands::OCPRecoveryCommands>(
            i2cBus, i2cAddress, false, false);

        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::OCPDeviceStatus);

        updateHealth();

        startWatchingSMAMCTPObjects(false);
    }

  private:
    std::unique_ptr<recovery_tool::recovery_commands::OCPRecoveryCommands>
        ocpRecoveryCommands;
    std::unique_ptr<BootStatus> bootStatus;
    std::vector<sdbusplus::bus::match_t> mctpSMAObjManagerMatch;
    std::unordered_map<std::string, std::string> mctpSMAEidObjects;
    std::vector<sdbusplus::bus::match_t> deviceSMAMatches;
    uint32_t smaEid;

    /* @brief Override function for updating Health and Status of D-Bus object
     * based on Device Status and MCTP enumeration
     * Uses OCP Recovery Protocol to fetch device status
     *
     * @return void
     */
    void updateHealth() override
    {
        const auto& [ret, output, _] =
            ocpRecoveryCommands->getDeviceStatusCommand();
        if (!ret)
        {
            lg2::error("Device associated with {PATH} is not accessible",
                       "PATH", path.c_str());

            bootStatus->bootStatus({0});
            health(HealthServer::HealthType::Critical);
            if (MCTPDiscoveryResource::isDeviceEnumerated())
            {
                state(OperationalStatusServer::StateType::UnavailableOffline);
                return;
            }

            state(OperationalStatusServer::StateType::Absent);
            return;
        }

        const auto status = static_cast<recovery_tool::DeviceStatus>(output[1]);
        bootStatus->bootStatus(
            std::vector<uint8_t>(output.begin() + 1, output.end()));

        if (MCTPDiscoveryResource::isDeviceEnumerated() and
            MCTPDiscoveryResource::checkForEnabledMCTPEids())
        {
            lg2::info("MCTP EID for {PATH} is enumerated and enabled", "PATH",
                      path.c_str());
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
            return;
        }

        if (status != recovery_tool::DeviceStatus::DeviceHealthy and
            status != recovery_tool::DeviceStatus::RecoveryImgRunning)
        {
            lg2::info("Device associated with {PATH} is in recovery", "PATH",
                      path.c_str());

            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::StandbyOffline);
            return;
        }

        lg2::info("Device associated with {PATH} is not in recovery", "PATH",
                  path.c_str());

        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
        return;
    }

    /**@brief Fetches a mapping of MCTP service to the list of EIDs associated
     * with the SMA resource
     *
     * @return Map between service name and mctp object path
     *
     */
    std::unordered_map<std::string, std::string> getSMAMCTPObjects()
    {
        std::unordered_map<std::string, std::string> mctpObjects{};
        const auto& mctpCtrlServices = getMctpServices();
        for (const auto& serviceName : mctpCtrlServices)
        {
            auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
            const auto objects = dbusUtil.getManagedObjects(
                serviceName.c_str(), mctpObjMgrPath.data());

            for (const auto& [objectPath, interfaces] : objects)
            {
                if (!interfaces.contains(mctpEndpointIntfName))
                {
                    continue;
                }

                const auto& mctpEID = std::get<uint8_t>(
                    interfaces.at(mctpEndpointIntfName).at("EID"));

                if (mctpEID != smaEid)
                {
                    continue;
                }
                mctpObjects[serviceName] = objectPath;
                break;
            }
        }
        return mctpObjects;
    }

    /**@brief Start watching for events on SMA MCTP objects
     *
     * @param needUpdateHealth - Whether to force a health status update
     *
     * @return void
     *
     */
    void startWatchingSMAMCTPObjects(bool needUpdateHealth)
    {
        // Clear any existing matches first to prevent accumulation
        mctpSMAObjManagerMatch.clear();
        deviceSMAMatches.clear();

        mctpSMAEidObjects = getSMAMCTPObjects();
        if (mctpSMAEidObjects.empty())
        {
            mctpSMAObjManagerMatch.emplace_back(
                bus, MatchRules::interfacesAdded(mctpObjMgrPath.data()),
                [&]([[maybe_unused]] sdbusplus::message::message& msg) {
                    startWatchingSMAMCTPObjects(true);
                });
            return;
        }

        mctpSMAObjManagerMatch.clear();

        for (const auto& [service, mctpObject] : mctpSMAEidObjects)
        {
            deviceSMAMatches.emplace_back(
                bus,
                MatchRules::propertiesChanged(mctpObject.c_str(),
                                              mctpEndpointEnableIntfName),
                std::bind(&GpuResource::onMCTPDiscoveryMsg, this,
                          std::placeholders::_1));

            deviceSMAMatches.emplace_back(
                bus, MatchRules::interfacesAdded(mctpObject.c_str()),
                std::bind(&GpuResource::onMCTPDiscoveryMsg, this,
                          std::placeholders::_1));
        }

        if (needUpdateHealth)
        {
            // Force a health status update since we might have missed the
            // signals during MCTP enumeration. The signals
            // (propertiesChanged/interfacesAdded) could have been sent before
            // we set up the matches above.
            updateHealth();
        }
    }
};
