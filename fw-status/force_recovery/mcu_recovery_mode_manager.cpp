/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "mcu_recovery_mode_manager.hpp"

#include <xyz/openbmc_project/Common/error.hpp>

namespace nvidia::recovery
{

MCURecoveryModeManager::MCURecoveryModeManager(
    sdbusplus::bus_t& bus, std::string chassisName, std::string objPath,
    const std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>&
        mcuRecoveryMgr,
    std::string usbPort) :
    RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath)),
    mcuRecoveryManager(mcuRecoveryMgr), usbPort(std::move(usbPort))
{
    lg2::info("MCURecoveryModeManager created for {CHASSIS}, USB port: {PORT}",
              "CHASSIS", this->chassisName, "PORT", this->usbPort);
}

void MCURecoveryModeManager::performForceRecovery()
{
    if (!mcuRecoveryManager)
    {
        lg2::error("MCU Recovery Manager not initialized");
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }

    lg2::info("Executing MCU force recovery for {CHASSIS} on port {PORT}",
              "CHASSIS", chassisName, "PORT", usbPort);

    try
    {
        // Temporarily initialize GPIO lines for recovery operation
        if (!mcuRecoveryManager->initGpioLines())
        {
            lg2::error("Failed to initialize GPIO lines for recovery");
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InternalFailure();
        }

        // Enter recovery mode for this specific MCU
        mcuRecoveryManager->enterRecoveryMode(usbPort);

        // Release GPIO lines after operation
        mcuRecoveryManager->releaseGpioLines();

        lg2::info("MCU recovery mode entered successfully for {CHASSIS}",
                  "CHASSIS", chassisName);
    }
    catch (const std::exception& e)
    {
        // Ensure GPIO is released even on error
        mcuRecoveryManager->releaseGpioLines();

        lg2::error("MCU recovery failed for {CHASSIS}: {ERR}", "CHASSIS",
                   chassisName, "ERR", e.what());
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
}

} // namespace nvidia::recovery
