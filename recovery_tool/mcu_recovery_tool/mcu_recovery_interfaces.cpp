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

#include "utils.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <thread>

using namespace mcu_recovery_manager;

int main(int argc, char* argv[])
{
    CLI::App app{"MCU Recovery Tool"};
    // Require at least one subcommand
    app.require_subcommand(1)->ignore_case();

    std::string jsonFilePath;
    std::string binaryFilePath;
    std::string target;
    std::string chassisName;
    bool forceUpdate = false;

    auto performRecovery =
        app.add_subcommand("PerformRecovery", "Perform recovery on the MCU");
    performRecovery->add_option("-f,--force", forceUpdate, "Force recovery");
    performRecovery->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (default: use Entity Manager D-Bus)");
    performRecovery->add_option("-t,--target", target,
                                "Target MCU type (e.g., CX9, HPM)");
    performRecovery->add_option("-c,--chassis", chassisName,
                                "Chassis name (e.g., IO_Board_SMA_0)");
    performRecovery
        ->add_option("-i,--image", binaryFilePath,
                     "Recovery image path (e.g., /path/to/image.bin)")
        ->required();

    auto forceReset = app.add_subcommand("ForceReset", "Force reset the MCU");
    forceReset->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (default: use Entity Manager D-Bus)");
    forceReset->add_option("-t,--target", target,
                           "Target MCU type (e.g., CX9, HPM)");
    forceReset->add_option("-c,--chassis", chassisName,
                           "Chassis name (e.g., IO_Board_SMA_0)");

    auto updateDeviceInfo =
        app.add_subcommand("GetDeviceStatus", "Get all device status");
    updateDeviceInfo->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (default: use Entity Manager D-Bus)");
    updateDeviceInfo->add_option("-t,--target", target,
                                 "Target MCU type (e.g., CX9, HPM)");
    updateDeviceInfo->add_option("-c,--chassis", chassisName,
                                 "Chassis name (e.g., IO_Board_SMA_0)");

    auto setForceRecovery = app.add_subcommand(
        "SetForceRecovery",
        "Force all MCUs into recovery mode (requires GPIO control)");
    setForceRecovery->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (default: use Entity Manager D-Bus)");
    setForceRecovery->add_option(
        "-t,--target", target, "Target MCU type (optional, default: all MCUs)");
    setForceRecovery->add_option("-c,--chassis", chassisName,
                                 "Chassis name (e.g., IO_Board_SMA_0)");

    MCURecoveryManager recoveryManager;

    // Add callbacks for PerformRecovery
    performRecovery->callback([&]() {
        std::map<std::string, MCUInfo> mcuConfig;

        if (!jsonFilePath.empty())
        {
            // Use JSON file if specified
            mcuConfig = parseJsonFile(jsonFilePath);
        }
        else
        {
            // Default: Use D-Bus Entity Manager
            // Priority: chassis > target > error
            if (!chassisName.empty())
            {
                mcuConfig = getMCUConfigByChassisFromDbus(chassisName);
                if (mcuConfig.size() != 1)
                {
                    lg2::error(
                        "Expected exactly 1 MCU for chassis {CHASSIS}, found {COUNT}",
                        "CHASSIS", chassisName, "COUNT", mcuConfig.size());
                    return;
                }
            }
            else if (!target.empty())
            {
                mcuConfig = getMCUConfigByTargetFromDbus(target);
            }
            else
            {
                lg2::error(
                    "--target or --chassis is required for PerformRecovery (unless using -j)");
                return;
            }
        }

        if (mcuConfig.empty())
        {
            lg2::error("No MCU configuration found");
            return;
        }

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performRecoveryFlow(binaryFilePath, forceUpdate);
    });

    // Add callback for ForceReset
    forceReset->callback([&]() {
        std::map<std::string, MCUInfo> mcuConfig;

        if (!jsonFilePath.empty())
        {
            // Use JSON file if specified
            mcuConfig = parseJsonFile(jsonFilePath);
        }
        else
        {
            // Default: Use D-Bus Entity Manager
            // Priority: chassis > target > all
            if (!chassisName.empty())
            {
                mcuConfig = getMCUConfigByChassisFromDbus(chassisName);
                if (mcuConfig.size() != 1)
                {
                    lg2::error(
                        "Expected exactly 1 MCU for chassis {CHASSIS}, found {COUNT}",
                        "CHASSIS", chassisName, "COUNT", mcuConfig.size());
                    return;
                }
            }
            else if (!target.empty())
            {
                mcuConfig = getMCUConfigByTargetFromDbus(target);
            }
            else
            {
                mcuConfig = getAllMCUConfigFromDbus();
            }
        }

        if (mcuConfig.empty())
        {
            lg2::error("No MCU configuration found");
            return;
        }

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performResetFlow();
    });

    // Add callback for GetDeviceStatus
    updateDeviceInfo->callback([&]() {
        std::map<std::string, MCUInfo> mcuConfig;

        if (!jsonFilePath.empty())
        {
            // Use JSON file if specified
            mcuConfig = parseJsonFile(jsonFilePath);
        }
        else
        {
            // Default: Use D-Bus Entity Manager
            // Priority: chassis > target > all
            if (!chassisName.empty())
            {
                mcuConfig = getMCUConfigByChassisFromDbus(chassisName);
                if (mcuConfig.size() != 1)
                {
                    lg2::error(
                        "Expected exactly 1 MCU for chassis {CHASSIS}, found {COUNT}",
                        "CHASSIS", chassisName, "COUNT", mcuConfig.size());
                    return;
                }
            }
            else if (!target.empty())
            {
                mcuConfig = getMCUConfigByTargetFromDbus(target);
            }
            else
            {
                mcuConfig = getAllMCUConfigFromDbus();
            }
        }

        if (mcuConfig.empty())
        {
            lg2::error("No MCU configuration found");
            return;
        }

        if (!recoveryManager.initialize(mcuConfig))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.updateAllDeviceInfo();
        recoveryManager.showAllDeviceStatus();
    });

    // Add callback for SetForceRecovery
    setForceRecovery->callback([&]() {
        std::map<std::string, MCUInfo> mcuConfig;

        if (!jsonFilePath.empty())
        {
            // Use JSON file if specified
            mcuConfig = parseJsonFile(jsonFilePath);
        }
        else
        {
            // Default: Use D-Bus Entity Manager
            // Priority: chassis > target > all
            if (!chassisName.empty())
            {
                mcuConfig = getMCUConfigByChassisFromDbus(chassisName);
                if (mcuConfig.size() != 1)
                {
                    lg2::error(
                        "Expected exactly 1 MCU for chassis {CHASSIS}, found {COUNT}",
                        "CHASSIS", chassisName, "COUNT", mcuConfig.size());
                    return;
                }
            }
            else if (!target.empty())
            {
                mcuConfig = getMCUConfigByTargetFromDbus(target);
            }
            else
            {
                mcuConfig = getAllMCUConfigFromDbus();
            }
        }

        if (mcuConfig.empty())
        {
            lg2::error("No MCU configuration found");
            return;
        }

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }

        lg2::info("Forcing {COUNT} MCU device(s) into recovery mode", "COUNT",
                  mcuConfig.size());

        // Step 1: Force all MCUs into recovery mode
        for (const auto& [usbPort, mcuInfo] : mcuConfig)
        {
            try
            {
                lg2::info("Forcing {DEV} into recovery mode", "DEV",
                          mcuInfo.device);
                recoveryManager.enterRecoveryMode(usbPort);
            }
            catch (const std::exception& e)
            {
                lg2::error("Failed to force {DEV} into recovery mode: {ERR}",
                           "DEV", mcuInfo.device, "ERR", e.what());
            }
        }

        // Step 2: Wait for USB re-enumeration
        lg2::info("Waiting for USB re-enumeration...");
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Step 3: Update device info
        recoveryManager.updateAllDeviceInfo();

        // Step 4: Verify all MCUs entered recovery mode
        for (const auto& [usbPort, mcuInfo] : mcuConfig)
        {
            if (recoveryManager.isInRecoveryMode(usbPort))
            {
                lg2::info("{DEV} successfully entered recovery mode", "DEV",
                          mcuInfo.device);
            }
            else
            {
                lg2::error(
                    "{DEV} failed to enter recovery mode (verification failed)",
                    "DEV", mcuInfo.device);
            }
        }
    });

    try
    {
        CLI11_PARSE(app, argc, argv);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}