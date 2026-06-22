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

#include "dbusutils.hpp"
#include "handler.hpp"
#include "mctp_discovery_resource.hpp"

#include <sdbusplus/timer.hpp>

#include <coroutine>

using namespace phosphor::logging;
class ERoTResource;

constexpr static int maxBootCompleteTimeoutPerAttempt = 120;
constexpr static int numberOfSlots = 2;
constexpr static int maxAttemptsPerSlot = 3;
constexpr static int maxBootCompleteTimeout =
    maxBootCompleteTimeoutPerAttempt * numberOfSlots * maxAttemptsPerSlot;

/**@class APResource
 *
 *  Represents a resource for Application Processor firmware monitoring
 *
 *  - Extends MCTPDiscoveryResource for MCTP endpoint discovery
 *  - Updates Health/State of the resource based on MCTP Events and BootStatus
 *
 */
class APResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the APResource class
     * Creates watchers for MCTP EIDs associated with the resource
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param apEid - MCTP Endpoint ID of the AP
     * @param erotResource - Pointer to the associated ERoT resource
     *
     */
    APResource(sdbusplus::bus_t& bus, const std::string& objPath, uint8_t apEid,
               ERoTResource* erotResource) :
        MCTPDiscoveryResource(bus, objPath, apEid), erotResource(erotResource)
    {
        initializeHealth().detach();
    }

    ~APResource() override
    {
        if (co && co.done())
        {
            co.destroy();
        }
    }

    /* @brief Function for updating Health and Status of the resource
     * based on QueryBootStatus and MCTP enumeration
     *
     * @return void
     */
    void updateHealth() override;

  private:
    std::unique_ptr<sdbusplus::Timer> timer;
    ERoTResource* erotResource;
    std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type> co;

    /**@brief Checks whether the AP FW is healthy based on the associated MCTP
     * EID object
     *
     * @return bool returns True when the AP FW is healthy,
     *              false otherwise
     *
     */
    inline bool isApHealthy() const noexcept
    {
        return isDeviceEnumerated();
    }

    /**@brief Checks whether the EC FW is healthy based on the ERoT MCTP EID
     * object
     *
     * @return bool returns True when the EC FW is healthy,
     *              false otherwise
     *
     */
    bool isERoTHealthy() const noexcept;

    /**@brief Updates the Health/State of the AP FW on D-Bus on boot complete.
     * Starts a timer for of duration max bootcomplete timeout if AP is found to
     * be in recovery
     *
     * @return coroutine
     *
     */
    mctp_vdm::requester::Coroutine initializeHealth();

    /**@brief Updates the Health/State of the AP FW on D-Bus object
     *
     * @return coroutine
     *
     */
    mctp_vdm::requester::Coroutine updateHealthAsync(bool chassisPoweredOff);

    /**@brief Checks whether the AP FW is in recovery based on QueryBootStatus
     * output and/or MCTP EID object
     *
     * @return bool True if AP is in recovery
     *              False otherwise
     *
     */
    bool isAPInRecovery() const noexcept;
};
