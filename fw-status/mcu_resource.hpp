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
#include "mcu_recovery_manager.hpp"

#include <libusb-1.0/libusb.h>

/**@class MCUResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the MCU Recovery Protocol
 *
 */
class MCUResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the MCUResource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param eid - MCTP Endpoint ID of the Resource
     * @param deviceId - MCU device identifier for the resource
     * @param mcuRecoveryManager - Shared pointer to MCU Recovery Manager
     *
     */
    MCUResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                uint8_t eid, const std::string& deviceId,
                std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>
                    mcuRecoveryManager) :
        MCTPDiscoveryResource(bus, objPath, eid), deviceId(deviceId),
        mcuRecoveryManager(mcuRecoveryManager)
    {
        lg2::info("Creating MCU Resource: EID={EID}, DEVICE_ID={ID}", "EID",
                  eid, "ID", deviceId);
    }

  private:
    std::string deviceId;
    std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>
        mcuRecoveryManager;

    /* @brief Override function for updating Health and Status of D-Bus object
     * based on device health and recovery mode status
     * Uses MCU Recovery Protocol to check device health and recovery mode
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

        (void)mcuRecoveryManager->updateDevInfo(deviceId);
        auto isHealthy = mcuRecoveryManager->isHealthy(deviceId);
        auto isInRecoveryMode = mcuRecoveryManager->isInRecoveryMode(deviceId);

        lg2::info(
            "Updating Health: DEVICE_ID={ID}, IS_HEALTHY={IS_HEALTHY}, IS_IN_RECOVERY_MODE={IS_IN_RECOVERY_MODE}",
            "ID", deviceId, "IS_HEALTHY", isHealthy, "IS_IN_RECOVERY_MODE",
            isInRecoveryMode);

        if (mctpEnumerated)
        {
            lg2::info("MCU device {ID} is healthy with MCTP enumerated", "ID",
                      deviceId);
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
            return;
        }

        if (isInRecoveryMode)
        {
            lg2::info("MCU device {ID} is in recovery mode", "ID", deviceId);
            commitRecoveryModeError(fetchEid());

            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::StandbyOffline);
            return;
        }

        if (isHealthy)
        {
            lg2::warning("MCU device {ID} is healthy but MCTP connectivity "
                         "is not available",
                         "ID", deviceId);
            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::Degraded);
            return;
        }

        lg2::error("MCU device {ID} is not accessible", "ID", deviceId);
        health(HealthServer::HealthType::Critical);

        if (MCTPDiscoveryResource::wasDeviceEnumeratedBefore())
        {
            state(OperationalStatusServer::StateType::UnavailableOffline);
        }
        else
        {
            state(OperationalStatusServer::StateType::Absent);
        }
    }
};
