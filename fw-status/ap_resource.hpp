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
#include "erot_resource.hpp"
#include "handler.hpp"
#include "mctp_vdm_helper.hpp"

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
 *  Represents a resource which is expected to have one or more associated MCTP
 * Endpoints, and is capable of triggering MCTP Discovery based on its health
 *
 *  - Updates Health/State of the resource based on MCTP Events and BootStatus
 *
 */
class APResource : public BaseResource
{
  public:
    /**@brief Constructor for the APResource class
     * Creates watchers for MCTP EIDs associated with the resource
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     *
     */
    APResource(sdbusplus::bus::bus& bus, const std::string& objPath,
               uint8_t apEid, ERoTResource* erotResource) :
        BaseResource(bus, objPath),
        eid(apEid), erotResource(erotResource)
    {
        startWatchingApEid();
        initializeHealth();
    }

    /* @brief Function for updating Health and Status of the resource
     * based on QueryBootStatus and MCTP enumeration
     *
     * @return void
     */
    void updateHealth()
    {

        if (co)
        {
            if (co.done())
            {
                co.destroy();
            }
            co = nullptr;
        }
        auto rc = updateHealthAsync();
        co = rc.handle;
        return;
    }

  private:
    uint8_t eid;
    std::string apMCTPService{};
    std::unique_ptr<sdbusplus::Timer> timer;
    std::vector<sdbusplus::bus::match_t> mctpApObjManagerMatch;
    ERoTResource* erotResource;
    std::vector<sdbusplus::bus::match_t> deviceMatches;
    std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type> co;

    /**@brief Callback function for MCTP event listeners
     * Updates Health and State of the D-Bus object
     *
     * @return void
     *
     */
    inline void onMCTPDiscoveryMsg(sdbusplus::message::message& msg)
    {
        lg2::info("MCTP Event received from Object: {OBJECT}, Updating Health",
                  "OBJECT", msg.get_path());
        updateHealth();
    }

    /**@brief Starts listening for events on AP MCTP EID object
     *
     * @return void
     *
     */
    void startWatchingApEid() noexcept;

    /**@brief Checks whether the associated MCTP EID object is Enabled
     *
     * @return bool returns True when the EID is Enabled,
     *              false otherwise
     *
     */
    bool checkForEnabledApEid() const noexcept
    {
        if (apMCTPService.empty())
        {
            return false;
        }

        const auto objPath =
            std::string(mctpObjPathPrefix) + std::to_string(eid);
        auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);

        auto ret =
            dbusUtil.getProperty<bool>(apMCTPService.c_str(), objPath.c_str(),
                                       mctpEndpointEnableIntfName, "Enabled");
        return ret;
    }

    /**@brief Checks whether the associated MCTP EID object is enumerated
     *
     * @return bool returns True when the EID is enumerated,
     *              false otherwise
     *
     */
    inline bool isApEnumerated() const noexcept
    {
        return !apMCTPService.empty();
    }

    /**@brief Checks whether the AP FW is healthy based on the associated MCTP
     * EID object
     *
     * @return bool returns True when the AP FW is healthy,
     *              false otherwise
     *
     */
    inline bool isApHealthy() const noexcept
    {
        return isApEnumerated() and checkForEnabledApEid();
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
    mctp_vdm::requester::Coroutine updateHealthAsync();

    /**@brief Checks whether the AP FW is in recovery based on QueryBootStatus
     * output and/or MCTP EID object
     *
     * @return bool True if AP is in recovery
     *              False otherwise
     *
     */
    bool isAPInRecovery() const noexcept;
};
