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

#include "base_resource.hpp"

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>

#include <memory>
#include <string_view>

constexpr static auto mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};
constexpr static auto mctpService{"au.com.codeconstruct.MCTP1"};
constexpr static std::string_view mctpObjPathPrefix =
    "/au/com/codeconstruct/mctp1/networks/1/endpoints/";
constexpr static std::string_view mctpObjMgrPath =
    "/au/com/codeconstruct/mctp1";

using namespace phosphor::logging;

/**@class MCTPDiscoveryResource
 *
 *  Represents a resource which is expected to have a single associated MCTP
 * Endpoint, and is capable of triggering MCTP Discovery based on its health
 *
 */
class MCTPDiscoveryResource : public BaseResource
{
  public:
    /**@brief Constructor for the MCTPDiscoveryResource class
     * Creates a watcher for the MCTP EID associated with the resource
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param eid - MCTP Endpoint ID of the Resource
     *
     */
    MCTPDiscoveryResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                          uint8_t eid) : BaseResource(bus, objPath), eid(eid)
    {
        monitorMCTPEndpoint();
    }

    /**@brief Fetches EID for the resource
     *
     * @return uint8_t - EID of the resource
     *
     */
    uint8_t fetchEid() const noexcept
    {
        return eid;
    }

    /**@brief Checks whether the resource has any associated MCTP endpoints
     * enumerated
     *
     * @return bool - True if the MCTP EID is enumerated,
     *                False otherwise
     *
     */
    inline bool isDeviceEnumerated() const noexcept
    {
        return !mctpObjectPath.empty();
    }

  protected:
    std::string mctpObjectPath;

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

  private:
    const uint8_t eid;
    std::unique_ptr<sdbusplus::bus::match_t> endpointAddedMatch;
    std::unique_ptr<sdbusplus::bus::match_t> endpointRemovedMatch;

    /**@brief Fetches the MCTP object path for the resource's EID
     *
     * @return string - MCTP object path, empty if not found
     *
     */
    std::string getMCTPObjectPath();

    /**@brief Updates the Health and State of the D-Bus Object
     *
     * @return void
     *
     */
    virtual void updateHealth() = 0;

    /**@brief Monitor MCTP endpoint for add/remove events
     */
    void monitorMCTPEndpoint();
};
