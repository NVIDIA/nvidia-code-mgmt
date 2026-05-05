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
                                             std::string configType) :
    RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath)),
    configType(std::move(configType))
{
    lg2::info("USBRCMRecoveryManager created for {CHASSIS}, config: {CFG}",
              "CHASSIS", this->chassisName, "CFG", this->configType);
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
    if (configType.empty())
    {
        lg2::error("ConfigType not specified for USB RCM recovery");
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }

    lg2::info("Executing USB RCM force recovery, config: {CFG}", "CFG",
              configType);

    nlohmann::json result;
    forceRecoveryMode(configType, result);

    // Log GPIO failures but don't fail the API - some boards may not be present
    if (!result.contains("Status") || result["Status"] != "Successful")
    {
        lg2::warning(
            "Some GPIO straps failed for {CHASSIS}: {ERR}. "
            "This may be due to platform misconfiguration or missing boards.",
            "CHASSIS", chassisName, "ERR",
            result.value("Error", "Unknown error"));
    }

    lg2::info("Force recovery straps set, waiting for device to enter recovery "
              "mode for {CHASSIS}",
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
            lg2::info("All devices confirmed in recovery mode for {CHASSIS}",
                      "CHASSIS", chassisName);
            break;
        }
    }

    lg2::info("Setting GPIO pins to default state for {CHASSIS}", "CHASSIS",
              chassisName);

    nlohmann::json coldBootResult;
    setGPIODefaultPinStates(configType, coldBootResult);

    // Log GPIO failures but don't fail the API - some boards may not be present
    if (!coldBootResult.contains("Status") ||
        coldBootResult["Status"] != "Successful")
    {
        lg2::warning(
            "Some GPIO default pin states failed for {CHASSIS}: {ERR}. "
            "This may be due to platform misconfiguration or missing boards.",
            "CHASSIS", chassisName, "ERR",
            coldBootResult.value("Error", "Unknown error"));
    }

    if (!deviceInRecovery)
    {
        lg2::error(
            "Not all devices confirmed recovery mode after {MAX} attempts for "
            "{CHASSIS}. GPIO default states set, but recovery status unconfirmed.",
            "MAX", maxRetries, "CHASSIS", chassisName);
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    else
    {
        lg2::info(
            "USB RCM recovery successful for {CHASSIS}, GPIO default states set",
            "CHASSIS", chassisName);
    }
}

} // namespace nvidia::recovery
