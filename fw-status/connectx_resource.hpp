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

#include "base_resource.hpp"
#include "dbusutils.hpp"
#include "i2c_utils.hpp"
#include "mctp_discovery_resource.hpp"
#include "utils.hpp"

#include <gpiod.hpp>

#include <array>
#include <format>
#include <memory>
#include <thread>

/**@class ConnectXResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the ConnectX Recovery Protocol
 *
 */
class ConnectXResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the ConnectXResource Class
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
     * @param resetGpioName - GPIO name for reset (e.g. from ResetGPIO config)
     * @param flashNotPresentGpioName - GPIO name for flash-not-present (e.g.
     * from FlashNotPresentGPIO config)
     */
    ConnectXResource(sdbusplus::bus_t& bus, const std::string& objPath,
                     const std::string& chassisObjPath,
                     const std::string& forceRecoveryChassisObjPath,
                     const uint64_t i2cBus, const uint64_t i2cAddress,
                     uint8_t eid, uint8_t smaEid,
                     const std::string& resetGpioName,
                     const std::string& flashNotPresentGpioName) :
        MCTPDiscoveryResource(bus, objPath, eid), smaEid(smaEid),
        busAddress(i2cBus), slaveAddress(i2cAddress),
        resetGpioName(resetGpioName),
        flashNotPresentGpioName(flashNotPresentGpioName)
    {
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::CXBootStatus);

        createRecoveryModeInterface(bus, forceRecoveryChassisObjPath);

        monitorSMAEndpoint();
    }

  private:
    std::unique_ptr<BootStatus> bootStatus;
    std::unique_ptr<SetRecoveryModeInterface> recoveryModeInterface;
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointAddedMatch;
    std::string smaMctpObjectPath;
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointRemovedMatch;
    int const connectxPublishDelayInSeconds = 5;

    uint8_t smaEid;
    int busAddress;
    int slaveAddress;
    std::string resetGpioName;
    std::string flashNotPresentGpioName;
    gpiod::line resetLine{};
    gpiod::line fnpLine{};

    static constexpr uint32_t resetActiveUs = 500000;
    static constexpr unsigned int resetDelaySec = 3;

    /**@brief Creates the SetRecoveryMode D-Bus interface on the chassis path
     *
     * @param bus - SystemD bus to publish the object
     * @param forceRecoveryChassisObjPath - Chassis D-Bus object path for the
     * SetRecoveryMode interface. If empty, the interface is not created.
     */
    void createRecoveryModeInterface(
        sdbusplus::bus_t& bus, const std::string& forceRecoveryChassisObjPath)
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
                lg2::info("Performing ConnectX force recovery for {PATH}",
                          "PATH", forceRecoveryChassisObjPath);

                auto [success, error] = setForceRecoveryMode();
                if (!success)
                {
                    lg2::error(
                        "ConnectX force recovery failed for {PATH}: {ERR}",
                        "PATH", forceRecoveryChassisObjPath, "ERR", error);
                    throw std::runtime_error(error);
                }

                lg2::info("ConnectX force recovery successful for {PATH}",
                          "PATH", forceRecoveryChassisObjPath);
            });
    }

    /** @brief ConnectX crspace address of irisc.global_image_status
     *
     * This array represents the ConnectX crspace address (0x50084) used to
     * access irisc.global_image_status. This register is used by the ConnectX
     * bootrom to report various failures in the bootrom flow before handing off
     * to BOOT2. The array format is [0, 0x05, 0x50, 0x84] which corresponds to
     * the address 0x50084.
     */
    static constexpr std::array<uint8_t, 4> writeDataArray = {0, 0x05, 0x50,
                                                              0x84};

    /* @brief Override function for updating Health and Status of D-Bus object
     * based on Device Status and MCTP enumeration
     * Uses ConnectX Recovery Protocol to fetch device status
     *
     * @return void
     */
    void updateHealth() override
    {
        const bool mctpEnumerated = MCTPDiscoveryResource::isDeviceEnumerated();

        if (!mctpEnumerated)
        {
            if (isChassisPoweredOff())
            {
                health(HealthServer::HealthType::Warning);
                state(OperationalStatusServer::StateType::UnavailableOffline);
                return;
            }
        }

        if (isPowerOnSettling() && !mctpEnumerated)
        {
            lg2::info("Deferring health update for {PATH} to settle timer",
                      "PATH", path.c_str());
            return;
        }

        const auto& [ret, output, errorMsg] = getDeviceStatus();
        if (!ret)
        {
            lg2::error(
                "Device associated with {PATH} is not accessible: {ERROR}",
                "PATH", path.c_str(), "ERROR", errorMsg);

            bootStatus->bootStatus({0});
            health(HealthServer::HealthType::Critical);
            if (MCTPDiscoveryResource::wasDeviceEnumeratedBefore())
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

        if (mctpEnumerated)
        {
            lg2::info("MCTP EID for {PATH} is enumerated", "PATH",
                      path.c_str());
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
            return;
        }

        if (inRecoveryState)
        {
            lg2::info("Device associated with {PATH} is in recovery", "PATH",
                      path.c_str());
            commitRecoveryModeError(fetchEid());

            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::StandbyOffline);
            return;
        }

        lg2::warning("Device associated with {PATH} is healthy but MCTP "
                     "connectivity is not available",
                     "PATH", path.c_str());

        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::Degraded);
        return;
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
                        sdbusplus::object_path addedPath;
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
                        sdbusplus::object_path removedPath;
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

    bool initGpioLines()
    {
        if (resetGpioName.empty() || flashNotPresentGpioName.empty())
        {
            return false;
        }
        try
        {
            resetLine = gpiod::find_line(resetGpioName);
            if (!resetLine)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           resetGpioName);
                return false;
            }
            fnpLine = gpiod::find_line(flashNotPresentGpioName);
            if (!fnpLine)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           flashNotPresentGpioName);
                return false;
            }
            resetLine.request({"connectx_force_recovery",
                               gpiod::line_request::DIRECTION_OUTPUT, 0},
                              1);
            fnpLine.request({"connectx_force_recovery",
                             gpiod::line_request::DIRECTION_OUTPUT, 0},
                            1);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to open GPIO {RESET}/{FNP}: {ERR}", "RESET",
                       resetGpioName, "FNP", flashNotPresentGpioName, "ERR",
                       e.what());
            return false;
        }
        return true;
    }

    void releaseGpioLines()
    {
        try
        {
            if (resetLine)
            {
                resetLine.release();
                resetLine = {};
            }
            if (fnpLine)
            {
                fnpLine.release();
                fnpLine = {};
            }
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "Failed to release GPIO for ConnectX force recovery: {ERR}",
                "ERR", e.what());
        }
    }

    void enterRecoveryMode()
    {
        if (!fnpLine || !resetLine)
        {
            lg2::error("ConnectX GPIO lines not initialized");
            return;
        }
        fnpLine.set_value(0);
        usleep(resetActiveUs);
        resetLine.set_value(0);
        usleep(resetActiveUs);
        resetLine.set_value(1);
        sleep(resetDelaySec);
    }

    std::pair<bool, std::string> setForceRecoveryMode()
    {
        if (resetGpioName.empty() || flashNotPresentGpioName.empty())
        {
            return {false, "GPIO not configured for force recovery (ResetGPIO/"
                           "FlashNotPresentGPIO)"};
        }

        if (!initGpioLines())
        {
            return {false,
                    "Failed to initialize GPIO lines for force recovery"};
        }

        try
        {
            enterRecoveryMode();
        }
        catch (const std::exception& e)
        {
            releaseGpioLines();
            return {false, "Failed to set GPIO for force recovery: " +
                               std::string(e.what())};
        }

        releaseGpioLines();
        return {true, "Successfully set force recovery mode"};
    }

  protected:
    /**
     * @brief Custom implementation of onMCTPDiscoveryMsg
     *
     * This function is a custom implementation of the onMCTPDiscoveryMsg
     * function. It is used to handle MCTP discovery messages for the ConnectX
     * resource.
     *
     * @param msg The message to handle
     */
    void onMCTPDiscoveryMsg(sdbusplus::message::message& msg)
    {
        lg2::info("MCTP Event received from Object: {OBJECT}, Updating Health",
                  "OBJECT", msg.get_path());

        std::this_thread::sleep_for(
            std::chrono::seconds(connectxPublishDelayInSeconds));
        updateHealth();
    }
};
