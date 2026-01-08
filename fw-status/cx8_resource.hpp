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
 * limitations under the License. */

#pragma once

#include "i2c_utils.hpp"
#include "mctp_discovery_resource.hpp"

#include <array>
#include <format>
#include <memory>
#include <thread>

/**@class Cx8Resource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the CX8 Recovery Protocol
 *
 */
class Cx8Resource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the Cx8Resource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param chassisObjPath - Path of D-Bus object to publish
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param eid - MCTP Endpoint ID of the Resource
     * @param smaEid - EID of the SMA
     */
    Cx8Resource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& chassisObjPath, const uint64_t i2cBus,
                const uint64_t i2cAddress, uint8_t eid, uint8_t smaEid) :
        MCTPDiscoveryResource(bus, objPath, eid), smaEid(smaEid),
        busAddress(i2cBus), slaveAddress(i2cAddress)
    {
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::CXBootStatus);

        updateHealth();

        startWatchingSMAMCTPObjects(false);
    }

  private:
    std::unique_ptr<BootStatus> bootStatus;
    std::vector<sdbusplus::bus::match_t> mctpSMAObjManagerMatch;
    std::unordered_map<std::string, std::string> mctpSMAEidObjects;
    std::vector<sdbusplus::bus::match_t> deviceSMAMatches;
    std::unique_ptr<sdbusplus::bus::match_t> chassisPowerStateMatch;
    std::string const chassisService = "xyz.openbmc_project.State.Chassis";
    std::string const chassisPath = "/xyz/openbmc_project/state/chassis0";
    std::string const chassisInterface = "xyz.openbmc_project.State.Chassis";
    std::string const chassisPowerStateOn =
        "xyz.openbmc_project.State.Chassis.PowerState.On";
    int const cx8PublishDelayInSeconds = 5;

    uint8_t smaEid;
    int busAddress;
    int slaveAddress;

    /** @brief CX8 crspace address of irisc.global_image_status
     *
     * This array represents the CX8 crspace address (0x50084) used to access
     * irisc.global_image_status. This register is used by the CX8 bootrom to
     * report various failures in the bootrom flow before handing off to BOOT2.
     * The array format is [0, 0x05, 0x50, 0x84] which corresponds to the
     * address 0x50084.
     */
    static constexpr std::array<uint8_t, 4> writeDataArray = {0, 0x05, 0x50,
                                                              0x84};

    /* @brief Override function for updating Health and Status of D-Bus object
     * based on Device Status and MCTP enumeration
     * Uses CX8 Recovery Protocol to fetch device status
     *
     * @return void
     */
    void updateHealth() override
    {
        const auto& [ret, output, _] = getDeviceStatus();
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

        bootStatus->bootStatus(output);

        // Check if device is in recovery state based on the boot status
        // 0x20000019 indicates normal operation
        bool inRecoveryState = (output[0] != 0x20 || output[1] != 0x00 ||
                                output[2] != 0x00 || output[3] != 0x19);

        if (inRecoveryState)
        {
            lg2::info("Device associated with {PATH} is in recovery", "PATH",
                      path.c_str());

            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::StandbyOffline);
            return;
        }

        if (MCTPDiscoveryResource::isDeviceEnumerated() and
            MCTPDiscoveryResource::checkForEnabledMCTPEids())
        {
            lg2::info("MCTP EID for {PATH} is enumerated and enabled", "PATH",
                      path.c_str());
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
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
                std::bind(&Cx8Resource::onMCTPDiscoveryMsg, this,
                          std::placeholders::_1));

            deviceSMAMatches.emplace_back(
                bus, MatchRules::interfacesAdded(mctpObject.c_str()),
                std::bind(&Cx8Resource::onMCTPDiscoveryMsg, this,
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

    /**
     * @brief Opens the I2C device for communication
     *
     * This function opens the I2C device specified by the bus address for
     * read/write operations. It constructs the device path using the format
     * "/dev/i2c-{busAddress}" and opens it with O_RDWR flags to enable both
     * reading and writing.
     *
     * @return CustomFD
     */
    utils::CustomFD openI2CDevice()
    {
        std::string i2cDevicePath = "/dev/i2c-" + std::to_string(busAddress);
        int fd = open(i2cDevicePath.c_str(), O_RDWR);
        if (fd < 0)
        {
            lg2::error("Failed to open I2C device: {ERROR}", "ERROR",
                       strerror(errno));
        }
        return utils::CustomFD(fd);
    }

    /**
     * @brief Retrieves the device's status.
     * @return A tuple containing success flag, status data as a byte vector,
     * and an error message if any.
     */
    std::tuple<bool, std::vector<uint8_t>, std::string> getDeviceStatus()
    {
        auto fd = openI2CDevice();
        if (fd() < 0)
        {
            return {false, {}, "Failed to open device."};
        }

        std::string errorMsg = "";
        std::vector<uint8_t> writeData(writeDataArray.begin(),
                                       writeDataArray.end());
        std::vector<uint8_t> readData(4);

        try
        {
            if (!recovery_tool::i2c_utils::sendI2cCmdForWriteRead(
                    fd(), static_cast<uint16_t>(slaveAddress), writeData,
                    readData, false))
            {
                return {false, {}, "Failed to get device status"};
            }
        }
        catch (const std::exception& e)
        {
            errorMsg = "Failed to get device status: " + std::string(e.what());
            return {false, {}, errorMsg};
        }

        return {true, readData, ""};
    }

  protected:
    /**
     * @brief Custom implementation of onMCTPDiscoveryMsg
     *
     * This function is a custom implementation of the onMCTPDiscoveryMsg
     * function. It is used to handle MCTP discovery messages for the CX8
     * resource.
     *
     * @param msg The message to handle
     */
    void onMCTPDiscoveryMsg(sdbusplus::message::message& msg)
    {
        lg2::info("MCTP Event received from Object: {OBJECT}, Updating Health",
                  "OBJECT", msg.get_path());

        std::this_thread::sleep_for(
            std::chrono::seconds(cx8PublishDelayInSeconds));
        updateHealth();
    }
};