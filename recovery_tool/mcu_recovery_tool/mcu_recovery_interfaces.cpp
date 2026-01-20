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
#include <stdexcept>
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
    bool useDbus = false;

    auto performRecovery =
        app.add_subcommand("PerformRecovery", "Perform recovery on the MCU");
    performRecovery->add_option("-f,--force", forceUpdate, "Force recovery");
    performRecovery->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (overrides -t/--target and -c/--chassis)");
    performRecovery->add_flag(
        "-e,--entity-manager", useDbus,
        "Use Entity Manager configuration on D-Bus (required when not using -j)");
    performRecovery->add_option(
        "-t,--target", target,
        "Target MCU type (e.g., CX9, HPM; ignored when using -j)");
    performRecovery->add_option(
        "-c,--chassis", chassisName,
        "Chassis name (e.g., IO_Board_SMA_0; ignored when using -j)");
    performRecovery
        ->add_option("-i,--image", binaryFilePath,
                     "Recovery image path (e.g., /path/to/image.bin)")
        ->required();

    auto forceReset = app.add_subcommand("ForceReset", "Force reset the MCU");
    forceReset->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (overrides -t/--target and -c/--chassis)");
    forceReset->add_flag(
        "-e,--entity-manager", useDbus,
        "Use Entity Manager configuration on D-Bus (required when not using -j)");
    forceReset->add_option(
        "-t,--target", target,
        "Target MCU type (e.g., CX9, HPM; ignored when using -j)");
    forceReset->add_option(
        "-c,--chassis", chassisName,
        "Chassis name (e.g., IO_Board_SMA_0; ignored when using -j)");

    auto updateDeviceInfo =
        app.add_subcommand("GetDeviceStatus", "Get all device status");
    updateDeviceInfo->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (overrides -t/--target and -c/--chassis)");
    updateDeviceInfo->add_flag(
        "-e,--entity-manager", useDbus,
        "Use Entity Manager configuration on D-Bus (required when not using -j)");
    updateDeviceInfo->add_option(
        "-t,--target", target,
        "Target MCU type (e.g., CX9, HPM; ignored when using -j)");
    updateDeviceInfo->add_option(
        "-c,--chassis", chassisName,
        "Chassis name (e.g., IO_Board_SMA_0; ignored when using -j)");

    auto setForceRecovery =
        app.add_subcommand("SetForceRecovery", "Force MCU into recovery mode");
    setForceRecovery->add_option(
        "-j,--json", jsonFilePath,
        "JSON configuration file (overrides -t/--target and -c/--chassis)");
    setForceRecovery->add_flag(
        "-e,--entity-manager", useDbus,
        "Use Entity Manager configuration on D-Bus (required when not using -j)");
    setForceRecovery->add_option(
        "-t,--target", target,
        "Target MCU type (optional, default: all MCUs; ignored when using -j)");
    setForceRecovery->add_option(
        "-c,--chassis", chassisName,
        "Chassis name (e.g., IO_Board_SMA_0; ignored when using -j)");

    // Helper function to get MCU configuration based on CLI options
    auto getMCUConfig = [&](bool allowAll) -> std::map<std::string, MCUInfo> {
        std::map<std::string, MCUInfo> mcuConfig;

        if (!jsonFilePath.empty())
        {
            // Use JSON file if specified
            // Error if user also specified entity-manager; warn for
            // target/chassis
            if (useDbus)
            {
                lg2::error(
                    "Specify exactly one of -j/--json or -e/--entity-manager");
                throw std::runtime_error("Invalid option selection");
            }
            if (!target.empty() || !chassisName.empty())
            {
                lg2::warning(
                    "Using JSON configuration file; -t/--target and -c/--chassis options are ignored");
            }
            mcuConfig = parseJsonFile(jsonFilePath);
        }
        else
        {
            if (!useDbus)
            {
                lg2::error(
                    "Specify exactly one of -j/--json or -e/--entity-manager");
                throw std::runtime_error("Invalid option selection");
            }
            // Use D-Bus Entity Manager
            // Priority: chassis > target > all (if allowed)
            if (!chassisName.empty())
            {
                mcuConfig = getMCUConfigByChassisFromDbus(chassisName);
                if (mcuConfig.size() != 1)
                {
                    lg2::error(
                        "Expected exactly 1 MCU for chassis {CHASSIS}, found {COUNT}. "
                        "Check Entity Manager configuration.",
                        "CHASSIS", chassisName, "COUNT", mcuConfig.size());
                    throw std::runtime_error("Invalid MCU configuration");
                }
            }
            else if (!target.empty())
            {
                mcuConfig = getMCUConfigByTargetFromDbus(target);
            }
            else if (allowAll)
            {
                mcuConfig = getAllMCUConfigFromDbus();
            }
            else
            {
                lg2::error(
                    "--target or --chassis is required (unless using -j)");
                throw std::runtime_error("Missing required option");
            }
        }

        if (mcuConfig.empty())
        {
            if (!jsonFilePath.empty())
            {
                lg2::error("No MCU configuration found in JSON file {FILE}",
                           "FILE", jsonFilePath);
            }
            else if (!target.empty())
            {
                lg2::error("No MCU configuration found for target {TARGET}",
                           "TARGET", target);
            }
            else
            {
                lg2::error("No MCU configuration found");
            }
            throw std::runtime_error("No MCU configuration found");
        }

        return mcuConfig;
    };

    MCURecoveryManager recoveryManager;

    // Add callbacks for PerformRecovery
    performRecovery->callback([&]() {
        // PerformRecovery requires explicit targeting (no "all devices"
        // fallback)
        auto mcuConfig = getMCUConfig(false);

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performRecoveryFlow(binaryFilePath, forceUpdate);
    });

    // Add callback for ForceReset
    forceReset->callback([&]() {
        // ForceReset allows "all devices" fallback
        auto mcuConfig = getMCUConfig(true);

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performResetFlow();
    });

    // Add callback for GetDeviceStatus
    updateDeviceInfo->callback([&]() {
        // GetDeviceStatus allows "all devices" fallback
        auto mcuConfig = getMCUConfig(true);

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
        // SetForceRecovery allows "all devices" fallback
        auto mcuConfig = getMCUConfig(true);

        if (!recoveryManager.initialize(mcuConfig, nullptr, true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }

        lg2::info("Forcing {COUNT} MCU device(s) into recovery mode", "COUNT",
                  mcuConfig.size());

        // Step 1: Force all MCUs into recovery mode
        try
        {
            recoveryManager.enterRecoveryModeAll();
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to force MCUs into recovery mode: {ERR}", "ERR",
                       e.what());
            return;
        }

        // Step 2: Wait for USB re-enumeration with polling
        lg2::info("Waiting for USB re-enumeration...");
        constexpr int maxRetries = 3;
        constexpr int retryIntervalMs = 1000;
        bool allDevicesReady = false;

        for (int attempt = 1; attempt <= maxRetries; ++attempt)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(retryIntervalMs));
            recoveryManager.updateAllDeviceInfo();

            size_t detectedCount = 0;
            for (const auto& [usbPort, mcuInfo] : mcuConfig)
            {
                if (recoveryManager.isInRecoveryMode(usbPort))
                {
                    ++detectedCount;
                }
            }

            if (detectedCount == mcuConfig.size())
            {
                allDevicesReady = true;
                lg2::info("All {COUNT} device(s) detected after {TIME}s",
                          "COUNT", mcuConfig.size(), "TIME", attempt);
                break;
            }
        }

        if (!allDevicesReady)
        {
            lg2::warning("USB re-enumeration timeout after {TIME}s", "TIME",
                         maxRetries);
        }

        // Step 3: Verify all MCUs entered recovery mode
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