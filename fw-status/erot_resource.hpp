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
    if (status.empty() || bit / 8 >= status.size())
    {
        return false;
    }

    size_t maxIdx = status.size() - 1;
    return (status[maxIdx - bit / 8] >> (bit % 8)) & 1;
}

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
     * @param eid - MCTP Endpoint ID of the Resource
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
                 const uint64_t i2cAddress, uint8_t eid, uint8_t apEid,
                 const std::string chassisObjPath, const std::string apObjPath,
                 const bool isRecoverable,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper);

    /**@brief Constructor for the ERoTResource Class
     * when the resource is not recoverable but publishes BootStatus
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param event - sdevent
     * @param eid - MCTP Endpoint ID of the Resource
     * @param mctpVdmHelper - MCTP VDM helper object
     * @param isRecoverable - Indicates whether recovery can be performed on the
     * Resource
     *
     */
    ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 sdeventplus::Event& event, uint8_t eid,
                 const std::string chassisObjPath, const bool isRecoverable,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper);

    ~ERoTResource() override
    {
        if (co && co.done())
        {
            co.destroy();
        }
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
        if (co)
        {
            if (co.done())
            {
                co.destroy();
            }
            else
            {
                co.promise().detached = true;
            }
            co = nullptr;
        }

        auto rc = updateBootStatusAsync();
        co = rc.handle;
        rc.handle = nullptr;

        if (co.done())
        {
            co.destroy();
            co = nullptr;
        }
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

    /** @brief Checks if AP boot is finished based on status bits
     *
     * @param status - Boot status vector
     * @return bool - True if boot is finished, false otherwise
     */
    bool isApBootFinished(const std::vector<uint8_t>& status);

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
    void updateERoTHealth();
};
