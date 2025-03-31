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

using namespace mcu_recovery_manager;

int main(int argc, char* argv[])
{
    CLI::App app{"MCU Recovery Tool"};
    // Require at least one subcommand
    app.require_subcommand(1)->ignore_case();

    std::string jsonFilePath;
    std::string binaryFilePath;
    bool forceUpdate = false;

    auto performRecovery =
        app.add_subcommand("PerformRecovery", "Perform recovery on the MCU");
    performRecovery->add_option("-f,--force", forceUpdate, "Force recovery");
    performRecovery->add_option("-j,--json", jsonFilePath,
                                "JSON configuration file");
    performRecovery
        ->add_option("-i,--image", binaryFilePath,
                     "Recovery image path (e.g., /path/to/image.bin)")
        ->required();

    auto forceReset = app.add_subcommand("ForceReset", "Force reset the MCU");
    forceReset->add_option("-j,--json", jsonFilePath,
                           "JSON configuration file");

    auto updateDeviceInfo =
        app.add_subcommand("GetDeviceStatus", "Get all device status");
    updateDeviceInfo->add_option("-j,--json", jsonFilePath,
                                 "JSON configuration file");

    MCURecoveryManager recoveryManager;

    // Add callbacks for PerformRecovery
    performRecovery->callback([&]() {
        if (!recoveryManager.initialize(parseJsonFile(jsonFilePath), nullptr,
                                        true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performRecoveryFlow(binaryFilePath, forceUpdate);
    });

    // Add callback for ForceReset
    forceReset->callback([&]() {
        if (!recoveryManager.initialize(parseJsonFile(jsonFilePath), nullptr,
                                        true))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.performResetFlow();
    });

    // Add callback for GetDeviceStatus
    updateDeviceInfo->callback([&]() {
        if (!recoveryManager.initialize(parseJsonFile(jsonFilePath)))
        {
            lg2::error("Failed to initialize recovery manager");
            return;
        }
        recoveryManager.updateAllDeviceInfo();
        recoveryManager.showAllDeviceStatus();
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