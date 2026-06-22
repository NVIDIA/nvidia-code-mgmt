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

#include "mctp_discovery_resource.hpp"

#include <memory>
#include <string>

class UdevMonitor;

/**@class USBRcmResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery status is determined
 *  through the USB RCM Recovery Protocol for NVIDIA Vera CPU devices.
 *
 *  Each instance creates TWO D-Bus objects:
 *  - Primary object (this resource) at
 * /xyz/openbmc_project/software/<component[0]>
 *  - Companion object (BaseResource) at
 * /xyz/openbmc_project/software/<component[1]>
 *
 *  Both objects share the same Health and OperationalStatus values.
 *
 */
class USBRcmResource : public MCTPDiscoveryResource
{
  public:
    /**@brief Constructor for the USBRcmResource Class
     * Updates Health and Status of the D-Bus object on startup
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish for the primary
     * component
     * @param eid - MCTP Endpoint ID of the Resource
     * @param usbPort - USB port path identifier for the resource
     *                  (e.g., "1-1.1.1.2")
     * @param companionObjPath - Path of D-Bus object for companion component
     * @param udevMonitor - Shared pointer to UdevMonitor for USB device events
     *
     */
    USBRcmResource(sdbusplus::bus_t& bus, const std::string& objPath,
                   uint8_t eid, const std::string& usbPort,
                   const std::string& companionObjPath,
                   std::shared_ptr<UdevMonitor> udevMonitor);

    ~USBRcmResource();

  private:
    std::string usbPort;
    std::shared_ptr<UdevMonitor> udevMonitor;
    std::unique_ptr<BaseResource> companionResource;

    /**@brief Override function for updating Health and Status of D-Bus object
     * based on device health and recovery mode status
     * Uses USB RCM Recovery Protocol to check device recovery status
     *
     * @return void
     */
    void updateHealth() override;

    /**@brief Query USB recovery status for this device
     * Calls getRecoveryStatus() and filters by USB port path
     *
     * @return Recovery status string ("Recovery Complete", "Not in Recovery",
     *         "In Recovery", "Unknown", or "USB Port Not Found")
     */
    std::string queryUSBRecoveryStatus();
};
