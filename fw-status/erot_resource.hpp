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

#pragma once

#include "ap_resource.hpp"
#include "glacier_recovery_commands.hpp"
#include "mctp_discovery_resource.hpp"
#include "mctp_vdm_helper.hpp"

#include <memory>
#include <mutex>

/*
 * This interval is used to keep polling boot status from ERoT until
 * either AP0_BOOT_COMPLETE or AP0_BOOT_COMPLETE_TIMEOUT is set
 * TODO: Set it via MESON option
 */
constexpr static int apBootCompleteRetryInterval = 10;
constexpr static size_t AP0_BOOT_COMPLETE_BIT = 5;
constexpr static size_t AP0_BOOT_COMPLETE_TIMEOUT_BIT = 27;

inline bool getBit(const std::vector<uint8_t>& status, size_t bit)
{
    size_t maxIdx = status.size() - 1;
    return (status[maxIdx - bit / 8] >> (bit % 8)) & 1;
}

class MCTPVdmHelper;
class APResource;

/**@class ERoTResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the Glacier Crisis recovery Protocol
 *
 *
 */
class ERoTResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the ERoTResource Class
     * when AP FW configuration is provided
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param event - sdevent
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param uuid - UUID of the Resource
     * @param apEid - EID of the AP associated with the ERoT
     * @param chassisObjPath - Path of the Chassis D-Bus object to publish
     * BootStatus
     * @param apObjPath - Path of the AP D-Bus object to publish Health/State
     * @param mctpVdmHelper - MCTP VDM helper object
     * @param isRecoverable - Indicates whether recovery can be performed on the
     * Resource
     *
     */
    ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 sdeventplus::Event& event, const uint64_t i2cBus,
                 const uint64_t i2cAddress, const std::string& uuid,
                 const uint64_t apEid, const std::string chassisObjPath,
                 const std::string apObjPath, const bool isRecoverable,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
        MCTPDiscoveryResource(bus, objPath, uuid),
        sdEvent(event), mctpVdmHelper(mctpVdmHelper),
        isRecoverable(isRecoverable)
    {
        glacierRecoveryObj =
            std::make_unique<glacier_recovery_tool::glacier_recovery_commands::
                                 GlacierRecoveryCommands>(i2cBus, i2cAddress,
                                                          false);
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatus({0});
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::ERoTBootStatus);
        apResource = std::make_unique<APResource>(bus, apObjPath, apEid, this);

        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);

        updateERoTHealth();

        apBootStatusTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(), [this, objPath]() {
                lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
                updateBootStatusAsync().detach();
            });
    }

    /**@brief Constructor for the ERoTResource Class
     * when the resource is not recoverable but publishes BootStatus
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param event - sdevent
     * @param uuid - UUID of the Resource
     * @param mctpVdmHelper - MCTP VDM helper object
     * @param isRecoverable - Indicates whether recovery can be performed on the
     * Resource
     *
     */
    ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 sdeventplus::Event& event, const std::string& uuid,
                 const std::string chassisObjPath, const bool isRecoverable,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
        MCTPDiscoveryResource(bus, objPath, uuid),
        sdEvent(event), mctpVdmHelper(mctpVdmHelper),
        isRecoverable(isRecoverable)
    {
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatus({0});
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::ERoTBootStatus);

        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);

        updateERoTHealth();

        apBootStatusTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(), [this, objPath]() {
                lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
                updateBootStatusAsync().detach();
            });
    }

    /**@brief Updates the BootStatus of the AP on chassis D-Bus object
     *
     * @return coroutine
     *
     */
    mctp_vdm::requester::Coroutine updateBootStatusAsync();

    /**@brief Updates the BootStatus D-Bus object
     *
     * @return coroutine
     *
     */
    void updateBootStatus()
    {
        if (co && !co.done())
        {
            lg2::info("Update in progress, skipping new update request");
            return;
        }

        if (co)
        {
            if (co.done())
            {
                co.destroy();
            }
            co = nullptr;
        }

        auto rc = updateBootStatusAsync();
        co = rc.handle;
    }

    std::vector<uint8_t> getBootStatus() const noexcept;

  private:
    sdeventplus::Event& sdEvent;
    std::unique_ptr<glacier_recovery_tool::glacier_recovery_commands::
                        GlacierRecoveryCommands>
        glacierRecoveryObj;
    std::mutex mtx;
    std::unique_ptr<APResource> apResource;
    std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
    std::unique_ptr<BootStatus> bootStatus;
    bool isRecoverable;
    std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type> co;
    std::unique_ptr<sdbusplus::Timer> apBootStatusTimer;

    /* @brief Override function for updating Health and Status of ERoT and AP
     * D-Bus objects based on Device Status and MCTP enumeration
     * Uses Glacier Crisis Recovery Protocol to fetch device status
     *
     * Updates Health/State of the AP FW if available
     *
     * @return void
     */
    void updateHealth() override
    {
        if (apResource)
        {
            apResource->updateHealth();
        }

        updateERoTHealth();
    }

    /* @brief Function for updating Health and Status of ERoT D-Bus object
     * based on Device Status and MCTP enumeration
     * Uses Glacier Crisis Recovery Protocol to fetch device status
     *
     * @return void
     */
    void updateERoTHealth()
    {
        if (bootStatus)
        {
            updateBootStatus();
        }

        if (!isRecoverable)
        {
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

        if (!glacierRecoveryObj->unlockI2CDevice())
        {
            lg2::error("Unable to unlock I2C for object {OBJECT}", "OBJECT",
                       path.c_str());
            health(HealthServer::HealthType::Critical);
            if (MCTPDiscoveryResource::isDeviceEnumerated())
            {
                state(OperationalStatusServer::StateType::UnavailableOffline);
                return;
            }

            state(OperationalStatusServer::StateType::Absent);
            return;
        }

        const auto& status = glacierRecoveryObj->performInitialization();

        if (status != glacier_recovery_tool::glacier_recovery_commands::
                          RecoveryResult::FirmwareNotInRecovery)
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

    /**
     * @brief Check if AP boot process is finished
     *
     * @param status A vector of bytes containing boot status information
     *
     * @return bool - Returns true if either bit 5 or bit 27 is set in the
     * status, false otherwise
     */
    bool isApBootFinished(const std::vector<uint8_t>& status);
};
