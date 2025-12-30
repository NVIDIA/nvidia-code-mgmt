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

#include "config.h"

#include "utils.hpp"

#include "dbusutils.hpp"

#include <sdbusplus/bus.hpp>

#include <filesystem>
#include <limits>
#include <optional>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto mcuRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.MCURecovery";

namespace mcu_recovery_manager
{

static auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

static nvidia::software::updater::GetSubTreeResponse getMCURecoverySubTree()
{
    nvidia::software::updater::GetSubTreeResponse getSubTreeResponse{};
    const nvidia::software::updater::Interfaces ifaceList{
        mcuRecoveryObjInterface};
    try
    {
        auto& bus = getBus();
        auto method = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetSubTree");
        method.append(entityManagerObjManager, 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("D-Bus error calling GetSubTree on ObjectMapper: {ERROR}",
                   "ERROR", e.what());
    }
    return getSubTreeResponse;
}

static std::optional<std::string>
    getPropertyString(const std::string& objectPath,
                      const std::string& property)
{
    try
    {
        auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
        return dbusUtil.getProperty<std::string>(
            entityManagerService, objectPath.c_str(), mcuRecoveryObjInterface,
            property.c_str());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::debug("Failed to read string property {PROP} for {PATH}: {ERR}",
                   "PROP", property, "PATH", objectPath, "ERR", e.what());
        return std::nullopt;
    }
}

static std::optional<uint64_t> getPropertyUint64(const std::string& objectPath,
                                                 const std::string& property)
{
    try
    {
        auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
        return dbusUtil.getProperty<uint64_t>(
            entityManagerService, objectPath.c_str(), mcuRecoveryObjInterface,
            property.c_str());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::debug("Failed to read uint64_t property {PROP} for {PATH}: {ERR}",
                   "PROP", property, "PATH", objectPath, "ERR", e.what());
        return std::nullopt;
    }
}

static std::optional<MCUInfo> populateMCUInfo(const std::string& objectPath,
                                              const std::string& deviceName)
{
    MCUInfo info;

    auto usbPortOpt = getPropertyString(objectPath, "USBPort");
    if (!usbPortOpt)
    {
        lg2::error("Missing USBPort property for {PATH}", "PATH", objectPath);
        return std::nullopt;
    }
    info.usbPort = usbPortOpt.value();

    auto resetGpioNameOpt = getPropertyString(objectPath, "ResetGpioName");
    if (!resetGpioNameOpt)
    {
        lg2::error("Missing ResetGpioName property for {PATH}", "PATH",
                   objectPath);
        return std::nullopt;
    }
    info.resetGpioName = resetGpioNameOpt.value();

    auto recoveryGpioNameOpt =
        getPropertyString(objectPath, "RecoveryGpioName");
    if (!recoveryGpioNameOpt)
    {
        lg2::error("Missing RecoveryGpioName property for {PATH}", "PATH",
                   objectPath);
        return std::nullopt;
    }
    info.recoveryGpioName = recoveryGpioNameOpt.value();

    auto functionalPidOpt = getPropertyUint64(objectPath, "ProductId");
    if (!functionalPidOpt)
    {
        lg2::error("Missing ProductId property for {PATH}", "PATH", objectPath);
        return std::nullopt;
    }

    // Validate ProductId fits in uint16_t range
    uint64_t pidValue = functionalPidOpt.value();
    if (pidValue > std::numeric_limits<uint16_t>::max())
    {
        lg2::error(
            "ProductId {PID} for {PATH} exceeds uint16_t range (max: 65535)",
            "PID", pidValue, "PATH", objectPath);
        return std::nullopt;
    }
    info.functionalPid = static_cast<uint16_t>(pidValue);

    info.device = deviceName;

    return info;
}

std::map<std::string, MCUInfo> parseJsonFile(const std::string& jsonFilePath)
{
    std::ifstream file(jsonFilePath);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open JSON file: " + jsonFilePath);
    }

    json j;
    file >> j;

    if (file.fail())
    {
        throw std::runtime_error("Failed to parse JSON file: " + jsonFilePath);
    }

    std::map<std::string, MCUInfo> mcuMap;
    for (const auto& item : j["Exposes"])
    {
        // Check if Type is MCURecovery and skip if not
        if (item["Type"] != "MCURecovery")
        {
            continue;
        }

        MCUInfo info;
        info.usbPort = item["USBPort"];
        info.resetGpioName = item["ResetGpioName"];
        info.recoveryGpioName = item["RecoveryGpioName"];
        info.functionalPid =
            std::stoi(item["ProductId"].get<std::string>(), nullptr, 16);
        info.device = item["Name"];
        mcuMap[info.usbPort] = info;
    }
    return mcuMap;
}

std::map<std::string, MCUInfo>
    getMCUConfigByTargetFromDbus(const std::string& target)
{
    const auto getSubTreeResponse = getMCURecoverySubTree();
    if (getSubTreeResponse.empty())
    {
        lg2::error("No devices found in Entity Manager");
        return {};
    }

    std::map<std::string, MCUInfo> mcuMap;

    for (const auto& [objectPath, _] : getSubTreeResponse)
    {
        auto targetOpt = getPropertyString(objectPath, "Target");
        if (!targetOpt)
        {
            continue;
        }

        // Skip if the target is not the one we want
        if (targetOpt.value() != target)
        {
            continue;
        }

        auto infoOpt = populateMCUInfo(
            objectPath, std::filesystem::path(objectPath).filename().string());
        if (!infoOpt)
        {
            continue;
        }

        mcuMap[infoOpt->usbPort] = infoOpt.value();
    }

    lg2::info(
        "Found {COUNT} MCU devices for target {TARGET} from Entity Manager",
        "COUNT", mcuMap.size(), "TARGET", target);

    return mcuMap;
}

std::map<std::string, MCUInfo> getAllMCUConfigFromDbus()
{
    const auto getSubTreeResponse = getMCURecoverySubTree();
    if (getSubTreeResponse.empty())
    {
        lg2::error("No devices found in Entity Manager");
        return {};
    }

    std::map<std::string, MCUInfo> mcuMap;

    for (const auto& [objectPath, _] : getSubTreeResponse)
    {
        auto infoOpt = populateMCUInfo(
            objectPath, std::filesystem::path(objectPath).filename().string());
        if (!infoOpt)
        {
            continue;
        }

        mcuMap[infoOpt->usbPort] = infoOpt.value();
    }

    lg2::info("Found {COUNT} MCU devices from Entity Manager", "COUNT",
              mcuMap.size());

    return mcuMap;
}

std::map<std::string, MCUInfo>
    getMCUConfigByChassisFromDbus(const std::string& chassisName)
{
    const auto getSubTreeResponse = getMCURecoverySubTree();
    if (getSubTreeResponse.empty())
    {
        lg2::error("No devices found in Entity Manager");
        return {};
    }

    std::map<std::string, MCUInfo> mcuMap;

    for (const auto& [objectPath, _] : getSubTreeResponse)
    {
        auto chassisNameOpt = getPropertyString(objectPath, "ChassisName");
        if (!chassisNameOpt)
        {
            continue;
        }

        // Skip if the chassis name is not the one we want
        if (chassisNameOpt.value() != chassisName)
        {
            continue;
        }

        auto infoOpt = populateMCUInfo(
            objectPath, std::filesystem::path(objectPath).filename().string());
        if (!infoOpt)
        {
            continue;
        }

        mcuMap[infoOpt->usbPort] = infoOpt.value();
    }

    lg2::info(
        "Found {COUNT} MCU device(s) for chassis {CHASSIS} from Entity Manager",
        "COUNT", mcuMap.size(), "CHASSIS", chassisName);

    return mcuMap;
}

std::string toHexString(uint16_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
        << value;
    return oss.str();
}

std::string executeCommand(const std::string& cmd)
{
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, void (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),
                                                [](FILE* f) { pclose(f); });

    if (!pipe)
    {
        throw std::runtime_error("popen() failed");
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        result += buffer.data();
    }

    return result;
}

bool isCommandSuccessful(const std::string& output)
{
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("Response status") != std::string::npos &&
            line.find("Success") != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace mcu_recovery_manager
