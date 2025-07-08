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
     * @param uuid - UUID of the Resource
     * @param usbPort - USB port identifier for the resource
     * @param mcuRecoveryManager - Shared pointer to MCU Recovery Manager
     *
     */
    MCUResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const std::string& uuid, const std::string& usbPort,
                std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>
                    mcuRecoveryManager) :
        MCTPDiscoveryResource(bus, objPath, uuid), usbPort(usbPort),
        mcuRecoveryManager(mcuRecoveryManager)
    {
        lg2::info("Creating MCU Resource: UUID={UUID}, USB_PORT={PORT}", "UUID",
                  uuid, "PORT", usbPort);

        updateHealth();
    }

  private:
    std::string usbPort;
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
        mcuRecoveryManager->updateAllDeviceInfo();
        auto isHealthy = mcuRecoveryManager->isHealthy(usbPort);
        auto isInRecoveryMode = mcuRecoveryManager->isInRecoveryMode(usbPort);

        lg2::info(
            "Updating Health: USB_PORT={PORT}, IS_HEALTHY={IS_HEALTHY}, IS_IN_RECOVERY_MODE={IS_IN_RECOVERY_MODE}",
            "PORT", usbPort, "IS_HEALTHY", isHealthy, "IS_IN_RECOVERY_MODE",
            isInRecoveryMode);

        if (isHealthy)
        {
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
        }
        else if (isInRecoveryMode)
        {
            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::StandbyOffline);
        }
        else
        {
            health(HealthServer::HealthType::Critical);

            if (MCTPDiscoveryResource::isDeviceEnumerated())
            {
                state(OperationalStatusServer::StateType::UnavailableOffline);
            }
            else
            {
                state(OperationalStatusServer::StateType::Absent);
            }
        }
    }
};
