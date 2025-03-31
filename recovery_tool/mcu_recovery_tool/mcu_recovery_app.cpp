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

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto mcuRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.MCURecovery";

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

static bool
    isMCURecoveryObject(nvidia::software::updater::InterfaceMap interfaces)
{
    if (interfaces.contains(mcuRecoveryObjInterface))
    {
        return true;
    }
    return false;
}

std::optional<std::string>
    getPropertyString(nvidia::software::updater::DBUSUtils& dbusUtil,
                      const std::string& objectPath,
                      const std::string& property)
{
    try
    {
        return dbusUtil.getProperty<std::string>(
            entityManagerService, objectPath.c_str(), mcuRecoveryObjInterface,
            property.c_str());
    }
    catch (const std::runtime_error& e)
    {
        return std::nullopt;
    }
}

std::optional<uint64_t>
    getPropertyUint64(nvidia::software::updater::DBUSUtils& dbusUtil,
                      const std::string& objectPath,
                      const std::string& property)
{
    try
    {
        return dbusUtil.getProperty<uint64_t>(
            entityManagerService, objectPath.c_str(), mcuRecoveryObjInterface,
            property.c_str());
    }
    catch (const std::runtime_error& e)
    {
        return std::nullopt;
    }
}

std::map<std::string, MCUInfo> getMCUConfig(const std::string& target)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);
    if (managedObjects.empty())
    {
        lg2::error("No devices found to recover");
        return {};
    }

    std::map<std::string, MCUInfo> mcuMap;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (isMCURecoveryObject(interfaces))
        {
            auto targetOpt =
                getPropertyString(dbusUtil, emObjectPath.str, "Target");
            if (!targetOpt)
            {
                return {};
            }

            // Skip if the target is not the one we want
            if (targetOpt.value() != target)
            {
                continue;
            }

            MCUInfo info;
            auto usbPortOpt =
                getPropertyString(dbusUtil, emObjectPath.str, "USBPort");
            if (!usbPortOpt)
            {
                return {};
            }
            info.usbPort = usbPortOpt.value();

            auto resetGpioNameOpt =
                getPropertyString(dbusUtil, emObjectPath.str, "ResetGpioName");
            if (!resetGpioNameOpt)
            {
                return {};
            }
            info.resetGpioName = resetGpioNameOpt.value();

            auto recoveryGpioNameOpt = getPropertyString(
                dbusUtil, emObjectPath.str, "RecoveryGpioName");
            if (!recoveryGpioNameOpt)
            {
                return {};
            }
            info.recoveryGpioName = recoveryGpioNameOpt.value();

            auto functionalPidOpt =
                getPropertyUint64(dbusUtil, emObjectPath.str, "ProductId");
            if (!functionalPidOpt)
            {
                return {};
            }
            info.functionalPid = functionalPidOpt.value();

            info.device = emObjectPath.filename();
            mcuMap[info.usbPort] = info;
        }
    }

    return mcuMap;
}

int main(int argc, char* argv[])
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
    auto mcuConfig = getMCUConfig(target);
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