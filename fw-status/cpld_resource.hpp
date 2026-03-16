/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION &
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

#include "base_resource.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <map>
#include <memory>
#include <string>

/** @class CpldResource
 *
 *  Represents a CPLD whose health is determined by two independent signals:
 *    1. A phosphor-logging entry for HPM-CPLD-AUTH-FAIL (auth failure)
 *    2. The MCTP endpoint availability of the SMA/MCU managing the CPLD
 *
 *  The auth-fail signal is produced by monitor-eventingd when gpio_monitor
 *  detects the CPLD auth-fail GPIO assertion. CpldResource consumes the
 *  resulting phosphor-logging entries rather than owning the GPIO line.
 *
 *  Health transitions:
 *    - Auth-fail log seen                 -> Critical / StandbyOffline
 *    - No auth-fail + SMA present         -> OK / Enabled
 *    - No auth-fail + SMA gone after seen -> Critical / UnavailableOffline
 *    - No auth-fail + SMA never seen      -> Critical / Absent
 */
class CpldResource : public BaseResource
{
  public:
    /** @brief Constructor
     *
     * @param bus      - SystemD bus to publish the object
     * @param objPath  - D-Bus object path to publish
     * @param smaEid   - MCTP EID of the SMA/MCU managing this CPLD
     * @param deviceId - monitor-eventing device ID (e.g. "CPLD_0")
     */
    CpldResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 uint8_t smaEid, const std::string& deviceId);

  private:
    // SMA/MCU MCTP endpoint tracking
    uint8_t smaEid;
    std::string smaEndpointObjectPath;
    bool hasObservedSMAEndpoint{false};
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointAddedMatch;
    std::unique_ptr<sdbusplus::bus::match_t> smaEndpointRemovedMatch;

    // Auth-fail tracking via phosphor-logging entries
    std::string deviceId;
    bool authFailed{false};
    std::unique_ptr<sdbusplus::bus::match_t> logEntryAddedMatch;

    /** @brief Query D-Bus for the SMA/MCU MCTP object path matching smaEid */
    std::string getSMAMCTPObjectPath();

    /** @brief Register D-Bus match rules to track SMA/MCU endpoint add/remove
     */
    void monitorSMAEndpoint();

    /** @brief Scan existing phosphor-logging entries for a prior auth-fail */
    void checkExistingAuthFail();

    /** @brief Subscribe to new phosphor-logging entries for auth-fail events */
    void monitorLoggingEvents();

    /** @brief Inspect AdditionalData from a log entry and update authFailed */
    void handleLogEntry(
        const std::map<std::string, std::string>& additionalData);

    /** @brief Read AdditionalData from a phosphor-logging entry object path */
    bool processLogEntry(const std::string& logEntryPath);

    /** @brief Recompute and publish Health + OperationalStatus */
    void updateHealth() override;
};
