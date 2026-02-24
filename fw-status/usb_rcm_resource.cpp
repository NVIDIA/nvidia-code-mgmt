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

#include "usb_rcm_resource.hpp"

#include "get_recovery_status.hpp"
#include "udev_monitor.hpp"

#include <phosphor-logging/lg2.hpp>

USBRcmResource::USBRcmResource(sdbusplus::bus::bus& bus,
                               const std::string& objPath, uint8_t eid,
                               const std::string& usbPort,
                               const std::string& companionObjPath,
                               std::shared_ptr<UdevMonitor> udevMonitor) :
    MCTPDiscoveryResource(bus, objPath, eid), usbPort(usbPort),
    udevMonitor(std::move(udevMonitor)),
    companionResource(std::make_unique<BaseResource>(bus, companionObjPath))
{
    lg2::info(
        "Creating USB RCM Resource: EID={EID}, USB_PORT={PORT}, PRIMARY={PRIMARY}, COMPANION={COMPANION}",
        "EID", eid, "PORT", usbPort, "PRIMARY", objPath, "COMPANION",
        companionObjPath);

    // Register for USB device add events to refresh health status
    if (udevMonitor)
    {
        udevMonitor->registerCallback(usbPort, [this]() { updateHealth(); });
    }

    updateHealth();
}

USBRcmResource::~USBRcmResource()
{
    if (udevMonitor)
    {
        udevMonitor->unregisterCallback(usbPort);
    }
}

std::string USBRcmResource::queryUSBRecoveryStatus()
{
    nlohmann::json jsonOutput;

    if (!getRecoveryStatus(jsonOutput, false))
    {
        lg2::error("Failed to query USB recovery status");
        return "Unknown";
    }

    if (jsonOutput.is_object() && jsonOutput.contains("Error"))
    {
        lg2::warning("USB recovery status query returned error: {ERR}", "ERR",
                     jsonOutput["Error"].get<std::string>());
        return "USB Port Not Found";
    }

    if (!jsonOutput.is_array())
    {
        lg2::error("Unexpected USB recovery status format");
        return "Unknown";
    }

    for (const auto& deviceEntry : jsonOutput)
    {
        if (!deviceEntry.contains("USB Port Path"))
        {
            continue;
        }

        const std::string portPath =
            deviceEntry["USB Port Path"].get<std::string>();
        if (portPath == usbPort)
        {
            if (deviceEntry.contains("Error") &&
                !deviceEntry["Error"].get<std::string>().empty())
            {
                lg2::warning("USB device at port {PORT} has error: {ERR}",
                             "PORT", usbPort, "ERR",
                             deviceEntry["Error"].get<std::string>());
                return "Unknown";
            }

            if (deviceEntry.contains("Recovery Status"))
            {
                return deviceEntry["Recovery Status"].get<std::string>();
            }

            return "Unknown";
        }
    }

    lg2::warning("USB device not found at port {PORT}", "PORT", usbPort);
    return "USB Port Not Found";
}

void USBRcmResource::updateHealth()
{
    if (isChassisPoweredOff())
    {
        health(HealthServer::HealthType::Warning);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        companionResource->health(HealthServer::HealthType::Warning);
        companionResource->state(
            OperationalStatusServer::StateType::UnavailableOffline);
        return;
    }

    const auto recoveryStatus = queryUSBRecoveryStatus();
    const bool mctpEnumerated = MCTPDiscoveryResource::isDeviceEnumerated();

    lg2::info(
        "Updating Health: USB_PORT={PORT}, RECOVERY_STATUS={STATUS}, MCTP_ENUMERATED={ENUM}",
        "PORT", usbPort, "STATUS", recoveryStatus, "ENUM", mctpEnumerated);

    HealthServer::HealthType healthValue;
    OperationalStatusServer::StateType stateValue;

    if (recoveryStatus == "In Recovery")
    {
        healthValue = HealthServer::HealthType::Critical;
        stateValue = OperationalStatusServer::StateType::StandbyOffline;
    }
    else if ((recoveryStatus == "Recovery Complete") ||
             (recoveryStatus == "Not in Recovery"))
    {
        if (mctpEnumerated)
        {
            healthValue = HealthServer::HealthType::OK;
            stateValue = OperationalStatusServer::StateType::Enabled;
        }
        else
        {
            lg2::warning("USB device at port {PORT} is healthy but MCTP "
                         "connectivity is not available",
                         "PORT", usbPort);
            healthValue = HealthServer::HealthType::Critical;
            stateValue = OperationalStatusServer::StateType::Degraded;
        }
    }
    else
    {
        // "Unknown" or "USB Port Not Found"
        healthValue = HealthServer::HealthType::Critical;
        stateValue =
            MCTPDiscoveryResource::wasDeviceEnumeratedBefore()
                ? OperationalStatusServer::StateType::UnavailableOffline
                : OperationalStatusServer::StateType::Absent;
    }

    health(healthValue);
    state(stateValue);

    companionResource->health(healthValue);
    companionResource->state(stateValue);
}
