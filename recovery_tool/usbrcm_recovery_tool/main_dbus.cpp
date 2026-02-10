/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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

#include "config.h"

#include "dbusutils.hpp"
#include "get_recovery_status.hpp"
#include "message_registry.hpp"
#include "perform_rcm_recovery.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace phosphor::logging;

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto usbRcmRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.USBRCMRecovery";

using ComponentId = uint16_t;
using ComponentName = std::string_view;

// Order in this array determines the flash sequence
static constexpr std::array<std::pair<ComponentId, ComponentName>, 9>
    componentMap = {{
        {0x2, "SBIOS_FMC"},
        {0x3, "L1_PT"},
        {0x204, "CSH-OEM_FWS"},
        {0x206, "Caliptra_FMC"},
        {0x205, "PSC_BCT"},
        {0x207, "CSH-NV_FWS"},
        {0x208, "OOBHUB_FW"},
        {0x209, "PSC_FW_Text"},
        {0x20A, "PSC_FW_Data"},
    }};

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

std::optional<std::string> getUSBPort(const std::string& objPath,
                                      const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    try
    {
        return dbusUtil.getProperty<std::string>(entityManagerService,
                                                 objPath.c_str(),
                                                 interface.c_str(), "USBPort");
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to get USBPort property: {ERR}", "ERR", e.what());
        return std::nullopt;
    }
}

/**
 * @brief Get DOT blob path for a device based on its name
 * @param deviceName Device name (e.g., "HGX_FW_CPU_0")
 * @return Path to DOT blob file if it exists, empty string otherwise
 */
std::string getDotBlobPath(const std::string& deviceName)
{
    std::string_view name = deviceName;
    constexpr std::string_view cpuPrefix = "CPU_";

    const auto pos = name.rfind(cpuPrefix);
    if (pos == std::string_view::npos)
        return "";

    const auto indexPos = pos + cpuPrefix.size();
    if (indexPos >= name.size())
        return "";

    std::string_view suffix = name.substr(indexPos);
    if (suffix.empty())
        return "";

    if (!std::all_of(suffix.begin(), suffix.end(), [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch));
        }))
        return "";

    constexpr std::string_view dotBlobDirectory = "/var/emmc/misc/dot-blob";
    std::filesystem::path dotBlobPath(dotBlobDirectory);
    dotBlobPath /= "CPU_" + std::string(suffix) + ".bin";

    return std::filesystem::exists(dotBlobPath) ? dotBlobPath.string() : "";
}

/**
 * @brief Find device entry in recovery status array by USB port path
 * @param devicesArray JSON array of device entries from getRecoveryStatus()
 * @param usbPort USB port path to search for
 * @return JSON object for matching device, or null if not found
 */
nlohmann::json findDeviceByPort(const nlohmann::json& devicesArray,
                                const std::string& usbPort)
{
    if (!devicesArray.is_array())
    {
        return nlohmann::json();
    }

    for (const auto& device : devicesArray)
    {
        if (device.contains("USB Port Path") &&
            device["USB Port Path"].get<std::string>() == usbPort)
        {
            return device;
        }
    }
    return nlohmann::json();
}

[[nodiscard]] constexpr ComponentName
    getComponentName(ComponentId compId) noexcept
{
    for (const auto& [id, name] : componentMap)
    {
        if (id == compId)
        {
            return name;
        }
    }
    return "Unknown";
}

struct RecoveryTask
{
    std::string deviceName;
    std::string usbPort;
};

struct RecoveryResult
{
    bool success;
    ErrorCode errorCode;
    std::string errorMessage;
    /** When true and success, Redfish registry uses enterDOTRecovery. */
    bool emptyDotBlobAccepted = false;
};

/**
 * @brief Sort and collect image paths by component ID
 * @param basePath Base path containing component directories
 * @return Vector of image paths in correct flash order
 */
std::vector<std::string> getOrderedImagePaths(const std::string& basePath)
{
    std::vector<std::string> orderedPaths;

    lg2::info("Scanning for recovery components in: {PATH}", "PATH", basePath);
    lg2::info("Expected {COUNT} components in flash order", "COUNT",
              componentMap.size());

    // PLDM extracts components to directories named with decimal component IDs
    // e.g., component 0x205 → directory "517"
    for (size_t i = 0; i < componentMap.size(); i++)
    {
        const auto& [compId, compName] = componentMap[i];
        std::filesystem::path componentDir = basePath;
        componentDir /= std::to_string(compId);

        if (!std::filesystem::exists(componentDir))
        {
            lg2::error(
                "[{SEQ}/{TOT}] Component 0x{ID:X} ({NAME}) NOT FOUND - directory '{DIR}' missing",
                "SEQ", i + 1, "TOT", componentMap.size(), "ID", compId, "NAME",
                compName, "DIR", componentDir.string());
            continue;
        }

        bool foundImage = false;
        for (const auto& entry :
             std::filesystem::directory_iterator(componentDir))
        {
            if (entry.is_regular_file())
            {
                orderedPaths.push_back(entry.path().string());
                lg2::info(
                    "[{SEQ}/{TOT}] Component 0x{ID:X} ({NAME}): {FILE} ({SIZE} bytes)",
                    "SEQ", i + 1, "TOT", componentMap.size(), "ID", compId,
                    "NAME", compName, "FILE", entry.path().filename().string(),
                    "SIZE", std::filesystem::file_size(entry.path()));
                foundImage = true;
                break;
            }
        }

        if (!foundImage)
        {
            lg2::error(
                "[{SEQ}/{TOT}] Component 0x{ID:X} ({NAME}) directory exists but NO IMAGE FILE found",
                "SEQ", i + 1, "TOT", componentMap.size(), "ID", compId, "NAME",
                compName);
        }
    }

    lg2::info("Component scan complete: {FOUND} of {EXPECTED} images ready",
              "FOUND", orderedPaths.size(), "EXPECTED", componentMap.size());

    return orderedPaths;
}

/**
 * @brief Execute recovery for a single device (thread-safe)
 *
 * This function is designed to be called from std::async for parallel
 * execution. It creates its own isolated USB context and does not share any
 * mutable state with other recovery tasks.
 *
 * @param task Recovery task containing device information
 * @param imagePaths Vector of image paths to flash (read-only, shared safely)
 * @return RecoveryResult with success/failure status and error details
 */
RecoveryResult executeRecoveryTask(const RecoveryTask& task,
                                   const std::vector<std::string>& imagePaths)
{
    RecoveryResult result;
    result.success = false;
    result.errorCode =
        static_cast<ErrorCode>(USBRCMRecoveryErrorCode::ImageTransferFailed);

    lg2::info("Starting recovery for device {DEVICE} on USB port {PORT}",
              "DEVICE", task.deviceName, "PORT", task.usbPort);

    try
    {
        std::string dotBlobPath = getDotBlobPath(task.deviceName);
        nlohmann::json recoveryOutput;
        if (!performUsbRecovery(task.usbPort, imagePaths, dotBlobPath,
                                recoveryOutput, false))
        {
            lg2::error("Firmware Recovery failed for Device: {DEVICE}",
                       "DEVICE", task.deviceName);

            if (recoveryOutput.contains("ErrorCode"))
            {
                result.errorCode = recoveryOutput["ErrorCode"].get<ErrorCode>();
                lg2::error("Recovery error code: {CODE} for device {DEVICE}",
                           "CODE", static_cast<unsigned>(result.errorCode),
                           "DEVICE", task.deviceName);
            }
            if (recoveryOutput.contains("Error"))
            {
                result.errorMessage =
                    recoveryOutput["Error"].get<std::string>();
                lg2::error("Error details for device {DEVICE}: {ERR}", "DEVICE",
                           task.deviceName, "ERR", result.errorMessage);
            }
            return result;
        }

        lg2::info("Device {DEVICE} successfully recovered", "DEVICE",
                  task.deviceName);
        result.success = true;
        result.errorCode = static_cast<ErrorCode>(0);
        if (recoveryOutput.contains("EmptyDotBlobAccepted") &&
            recoveryOutput["EmptyDotBlobAccepted"].get<bool>())
        {
            result.emptyDotBlobAccepted = true;
        }
        return result;
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Firmware Recovery failed for Device: {DEVICE}, Error: {ERROR}",
            "DEVICE", task.deviceName, "ERROR", e.what());
        result.errorMessage = e.what();
        result.errorCode = static_cast<ErrorCode>(deviceRecoveryFailed);
        return result;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        lg2::error("Invalid number of arguments. Usage: usb-rcm-recovery "
                   "<image_base_path>");
        return -1;
    }

    auto& bus = getBus();
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());

    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);

    std::unique_ptr<MessageRegistry> messageRegistry =
        std::make_unique<MessageRegistry>(bus);

    if (managedObjects.empty())
    {
        lg2::error("No Devices found to recover");
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
            static_cast<ErrorCode>(noDevicesFound), "USBRCMRecovery");
        return -1;
    }

    nlohmann::json allDevicesStatus = nlohmann::json::array();
    if (!getRecoveryStatus(allDevicesStatus, false))
    {
        lg2::error("Failed to get recovery status from USB devices");
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
            static_cast<ErrorCode>(USBRCMRecoveryErrorCode::DeviceNotFound),
            "USBRCMRecovery");
        return -1;
    }

    int recoveryTaskState = 0;
    std::string imageBasePath = argv[1];

    lg2::info("Scanning component directories for latest image files...");
    auto imagePaths = getOrderedImagePaths(imageBasePath);

    if (imagePaths.size() != componentMap.size())
    {
        lg2::error(
            "Invalid component count: found {FOUND}, expected {EXPECTED} for Vera recovery",
            "FOUND", imagePaths.size(), "EXPECTED", componentMap.size());
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
            static_cast<ErrorCode>(USBRCMRecoveryErrorCode::FileOpenFailure),
            "USBRCMRecovery");
        return -1;
    }

    lg2::info("Found all {COUNT} required Vera recovery images", "COUNT",
              imagePaths.size());

    for (size_t i = 0; i < imagePaths.size(); i++)
    {
        lg2::info("  Image {NUM}: {FILE}", "NUM", i + 1, "FILE",
                  std::filesystem::path(imagePaths[i]).filename().string());
    }

    std::vector<RecoveryTask> recoveryTasks;

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (!interfaces.contains(usbRcmRecoveryObjInterface))
        {
            continue;
        }

        lg2::info("Found USB RCM recovery config Object: {PATH}", "PATH",
                  emObjectPath);

        const auto& device = emObjectPath.filename();

        const auto usbPortOpt =
            getUSBPort(emObjectPath.str, usbRcmRecoveryObjInterface);
        if (!usbPortOpt.has_value() || usbPortOpt->empty())
        {
            lg2::error("USBPort property not found or empty for {DEVICE}",
                       "DEVICE", device);
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
                deviceNotResponding, device);
            recoveryTaskState = -1;
            continue;
        }
        const auto& usbPort = usbPortOpt.value();

        lg2::info("Device {DEVICE} configured on USB port: {PORT}", "DEVICE",
                  device, "PORT", usbPort);

        nlohmann::json statusOutput =
            findDeviceByPort(allDevicesStatus, usbPort);
        if (statusOutput.empty())
        {
            lg2::error(
                "Device {DEVICE} not found in USB recovery status (port: {PORT})",
                "DEVICE", device, "PORT", usbPort);
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
                static_cast<ErrorCode>(USBRCMRecoveryErrorCode::DeviceNotFound),
                device);
            recoveryTaskState = -1;
            continue;
        }

        std::string recoveryStatus =
            statusOutput.value("Recovery Status", "Unknown");

        if (recoveryStatus == "Not in Recovery")
        {
            lg2::info("Device {DEVICE} is healthy, skipping recovery", "DEVICE",
                      device);
            messageRegistry->createMessageRegistry(firmwareNotInRecovery,
                                                   device);
            continue;
        }

        if (recoveryStatus == "Recovery Complete")
        {
            lg2::info("Device {DEVICE} recovery already complete, skipping",
                      "DEVICE", device);
            messageRegistry->createMessageRegistry(recoverySuccessful, device);
            continue;
        }

        if (recoveryStatus != "In Recovery")
        {
            lg2::error(
                "Device {DEVICE} not in recovery mode (status: {STATUS}), skipping",
                "DEVICE", device, "STATUS", recoveryStatus);
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
                deviceNotResponding, device);
            recoveryTaskState = -1;
            continue;
        }

        RecoveryTask task;
        task.deviceName = device;
        task.usbPort = usbPort;
        recoveryTasks.push_back(std::move(task));

        lg2::info("Device {DEVICE} queued for recovery", "DEVICE", device);
        messageRegistry->createMessageRegistry(recoveryStarted, device);
    }

    if (recoveryTasks.empty())
    {
        lg2::info("No devices require recovery");
        return recoveryTaskState;
    }

    lg2::info("Starting recovery for {COUNT} device(s)", "COUNT",
              recoveryTasks.size());

    std::map<std::string, std::future<RecoveryResult>> futures;
    for (const auto& task : recoveryTasks)
    {
        futures.emplace(task.deviceName,
                        std::async(std::launch::async, executeRecoveryTask,
                                   task, std::cref(imagePaths)));
    }

    lg2::info("Waiting for {COUNT} recovery task(s) to complete...", "COUNT",
              futures.size());

    size_t successCount = 0;
    size_t failCount = 0;

    for (auto& [deviceName, future] : futures)
    {
        try
        {
            RecoveryResult result = future.get();
            if (result.success)
            {
                lg2::info("Recovery completed successfully for {DEVICE}",
                          "DEVICE", deviceName);
                if (result.emptyDotBlobAccepted)
                {
                    messageRegistry->createMessageRegistry(enterDOTRecovery,
                                                           deviceName);
                }
                else
                {
                    messageRegistry->createMessageRegistry(recoverySuccessful,
                                                           deviceName);
                }
                successCount++;
            }
            else
            {
                lg2::error("Recovery failed for {DEVICE}: {ERR}", "DEVICE",
                           deviceName, "ERR", result.errorMessage);
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
                    result.errorCode, deviceName);
                recoveryTaskState = -1;
                failCount++;
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Exception while waiting for recovery task for {DEVICE}: {ERR}",
                "DEVICE", deviceName, "ERR", e.what());
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBRCMRecovery,
                static_cast<ErrorCode>(deviceRecoveryFailed), deviceName);
            recoveryTaskState = -1;
            failCount++;
        }
    }

    lg2::info("Recovery complete: {SUCCESS} succeeded, {FAIL} failed",
              "SUCCESS", successCount, "FAIL", failCount);

    return recoveryTaskState;
}
