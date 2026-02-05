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

#include "dbusutils.hpp"
#include "utils.hpp"

#include <sdbusplus/bus.hpp>

#include <optional>

using namespace mcu_recovery_manager;

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc < 3)
        {
            lg2::error(
                "Invalid number of arguments. Usage: mcu-recovery $BINARY $TARGET");
            return -1;
        }

        std::string binaryFilePath = argv[1];
        std::string target = argv[2];

        MCURecoveryManager recoveryManager;
        auto mcuConfig = getMCUConfigByTargetFromDbus(target);
        auto messageRegistry = std::make_unique<MessageRegistry>(getBus());

        if (mcuConfig.empty())
        {
            lg2::error("Cannot successfully retrieve MCU config for {TARGET}",
                       "TARGET", target);
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                static_cast<ErrorCode>(noDevicesFound), "MCURecovery");
            return -1;
        }

        if (!recoveryManager.initialize(mcuConfig, std::move(messageRegistry),
                                        true))
        {
            lg2::error("Failed to initialize recovery manager");
            return -1;
        }
        recoveryManager.performRecoveryFlow(binaryFilePath, false);

        return 0;
    }
    catch (const std::exception& e)
    {
        lg2::error("Error during MCU recovery: {ERROR}", "ERROR", e.what());
        return -1;
    }

    return 0;
}