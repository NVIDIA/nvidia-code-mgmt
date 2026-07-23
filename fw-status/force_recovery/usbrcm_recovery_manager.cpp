/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "usbrcm_recovery_manager.hpp"

#include "force_recovery.hpp"
#include "get_recovery_status.hpp"

#include <nlohmann/json.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <chrono>
#include <thread>

namespace nvidia::recovery
{

static constexpr int maxRetries = 10;
static constexpr int delayBetweenRetriesSec = 1;

USBRCMRecoveryManager::USBRCMRecoveryManager(sdbusplus::bus_t& bus,
                                             std::string chassisName,
                                             std::string objPath,
                                             RecoveryPinConfig pinConfig) :
    RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath)),
    pinConfig(std::move(pinConfig))
{
    lg2::info("USBRCMRecoveryManager created for {CHASSIS}", "CHASSIS",
              this->chassisName);
}

bool USBRCMRecoveryManager::areDevicesInRecovery() const
{
    nlohmann::json statusResult;
    if (!getRecoveryStatus(statusResult, false))
    {
        lg2::error("Failed to get recovery status from USB devices");
        return false;
    }

    if (!statusResult.is_array())
    {
        lg2::error("Failed to parse recovery status from USB devices");
        return false;
    }

    if (statusResult.empty())
    {
        lg2::error("No USB devices found for recovery status check");
        return false;
    }

    size_t devicesInRecovery = 0;
    for (const auto& device : statusResult)
    {
        if (device.contains("Recovery Status"))
        {
            std::string status = device["Recovery Status"].get<std::string>();
            if (status == "In Recovery")
            {
                devicesInRecovery++;
            }
            else
            {
                lg2::info(
                    "Device not in recovery mode: {PORT}, status: {STATUS}",
                    "PORT", device.value("USB Port Path", "unknown"), "STATUS",
                    status);
            }
        }
    }

    lg2::info("Recovery status: {COUNT}/{TOTAL} devices in recovery mode",
              "COUNT", devicesInRecovery, "TOTAL", statusResult.size());

    return devicesInRecovery == statusResult.size();
}

void USBRCMRecoveryManager::performForceRecovery()
{
    lg2::info("Executing USB RCM force recovery for {CHASSIS}", "CHASSIS",
              chassisName);

    nlohmann::json result;
    forceRecoveryMode(pinConfig, result);

    if (!result.contains("Status") || result["Status"] != "Successful")
    {
        lg2::error("Force recovery straps failed for {CHASSIS}: {ERR}",
                   "CHASSIS", chassisName, "ERR",
                   result.value("Error", "Unknown error"));
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }

    lg2::info("Straps set, waiting for {CHASSIS} to enter recovery mode",
              "CHASSIS", chassisName);

    bool deviceInRecovery = false;
    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        std::this_thread::sleep_for(
            std::chrono::seconds(delayBetweenRetriesSec));

        lg2::info("Checking recovery status, attempt {ATTEMPT}/{MAX}",
                  "ATTEMPT", attempt + 1, "MAX", maxRetries);

        if (areDevicesInRecovery())
        {
            deviceInRecovery = true;
            lg2::info("All devices in recovery mode for {CHASSIS}", "CHASSIS",
                      chassisName);
            break;
        }
    }

    nlohmann::json restoreResult;
    setGPIODefaultPinStates(pinConfig, restoreResult);

    if (!restoreResult.contains("Status") ||
        restoreResult["Status"] != "Successful")
    {
        lg2::error("GPIO restore failed for {CHASSIS}: {ERR}", "CHASSIS",
                   chassisName, "ERR",
                   restoreResult.value("Error", "Unknown error"));
    }

    if (!deviceInRecovery)
    {
        lg2::error(
            "No devices confirmed recovery mode after {MAX} attempts for "
            "{CHASSIS}",
            "MAX", maxRetries, "CHASSIS", chassisName);
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
}

} // namespace nvidia::recovery
