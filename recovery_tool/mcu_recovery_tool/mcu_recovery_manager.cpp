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

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <CLI/CLI.hpp>

#include <cerrno>
#include <cstring>
#include <list>
#include <sstream>

using namespace mcu_recovery_manager;

namespace mcu_recovery_manager
{

MCURecoveryManager::MCURecoveryManager() : context(nullptr)
{}

MCURecoveryManager::~MCURecoveryManager()
{
    if (context)
    {
        libusb_exit(context);
    }
}

bool MCURecoveryManager::initialize(
    const std::map<std::string, MCUInfo>& mcuMap,
    std::unique_ptr<MessageRegistry> messageRegistry, bool initGpio)
{
    this->mcuMap = mcuMap;
    if (messageRegistry)
    {
        this->messageRegistry = std::move(messageRegistry);
    }

    const bool hasUsb = std::any_of(
        this->mcuMap.begin(), this->mcuMap.end(), [](const auto& item) {
            return item.second.interfaceType == MCUInfo::InterfaceType::USB;
        });
    if (hasUsb && libusb_init(&context) < 0)
    {
        lg2::error("Failed to initialize libusb");
        return false;
    }
    if (initGpio && !initGpioLines())
    {
        lg2::error("Failed to initialize GPIO lines");
        return false;
    }
    return true;
}

std::string MCURecoveryManager::getI2cTarget(uint8_t bus, uint16_t address)
{
    std::ostringstream oss;
    oss << "/dev/i2c-" << static_cast<unsigned>(bus) << ",0x" << std::hex
        << std::uppercase << address;
    return oss.str();
}

bool MCURecoveryManager::probeI2cAddress(const std::string& deviceId,
                                         uint16_t address)
{
    const auto& info = mcuMap[deviceId];
    std::string devPath = "/dev/i2c-" + std::to_string(info.i2cBus);

    int fd = open(devPath.c_str(), O_RDWR);
    if (fd < 0)
    {
        lg2::error("Failed to open {DEV}: {ERR}", "DEV", devPath, "ERR",
                   std::strerror(errno));
        return false;
    }

    if (ioctl(fd, I2C_SLAVE, address) < 0)
    {
        lg2::error("Failed to set I2C address 0x{ADDR} on {DEV}: {ERR}", "ADDR",
                   toHexString(address), "DEV", devPath, "ERR",
                   std::strerror(errno));
        close(fd);
        return false;
    }

    i2c_smbus_ioctl_data ioctlData{};
    i2c_smbus_data smbusData{};
    ioctlData.read_write = I2C_SMBUS_WRITE;
    ioctlData.command = 0;
    ioctlData.size = I2C_SMBUS_QUICK;
    ioctlData.data = &smbusData;
    int ret = ioctl(fd, I2C_SMBUS, &ioctlData);
    if (ret < 0)
    {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool MCURecoveryManager::updateI2cDevInfo(const std::string& deviceId)
{
    auto& device = mcuDevices[deviceId];
    const auto& info = mcuMap[deviceId];

    device.i2cHealthy = probeI2cAddress(deviceId, info.normalI2cAddress);
    device.inRecoveryMode = probeI2cAddress(deviceId, info.recoveryI2cAddress);

    if (device.i2cHealthy || device.inRecoveryMode)
    {
        return true;
    }

    lg2::error("{DEV} not found on I2C bus {BUS}", "DEV", info.device, "BUS",
               info.i2cBus);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::MCURecovery,
            noDevicesFound, info.device);
    }
    return false;
}

bool MCURecoveryManager::initGpioLines()
{
    for (auto& [deviceId, mcuInfo] : mcuMap)
    {
        try
        {
            mcuDevices[deviceId].resetPin =
                gpiod::find_line(mcuInfo.resetGpioName);
            if (!mcuDevices[deviceId].resetPin)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           mcuInfo.resetGpioName);
                continue;
            }
            mcuDevices[deviceId].recoveryPin =
                gpiod::find_line(mcuInfo.recoveryGpioName);
            if (!mcuDevices[deviceId].recoveryPin)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           mcuInfo.recoveryGpioName);
                continue;
            }
            // set the default value of the reset pin to 1
            mcuDevices[deviceId].resetPin.request(
                {"mcu_recovery", gpiod::line_request::DIRECTION_OUTPUT, 0}, 1);
            mcuDevices[deviceId].recoveryPin.request(
                {"mcu_recovery", gpiod::line_request::DIRECTION_OUTPUT, 0}, 1);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to open GPIO {RESET}/{RECOVERY}: {ERR}", "RESET",
                       mcuInfo.resetGpioName, "RECOVERY",
                       mcuInfo.recoveryGpioName, "ERR", e.what());
            return false;
        }
    }
    return true;
}

void MCURecoveryManager::releaseGpioLines()
{
    for (auto& [deviceId, device] : mcuDevices)
    {
        try
        {
            if (device.resetPin)
            {
                device.resetPin.release();
            }
            if (device.recoveryPin)
            {
                device.recoveryPin.release();
            }
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "Failed to release GPIO for {DEV}: {ERR}, continuing...", "DEV",
                mcuMap[deviceId].device, "ERR", e.what());
        }
    }
}

void MCURecoveryManager::enterRecoveryMode(const std::string& deviceId)
{
    lg2::info("{DEV} entering recovery mode...", "DEV",
              mcuMap[deviceId].device);
    if (!mcuDevices[deviceId].recoveryPin || !mcuDevices[deviceId].resetPin)
    {
        lg2::error("Skipping {DEV}: GPIO lines not initialized", "DEV",
                   mcuMap[deviceId].device);
        return;
    }
    mcuDevices[deviceId].recoveryPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[deviceId].resetPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[deviceId].resetPin.set_value(1);
    sleep(mcuResetDelaySec);
}

void MCURecoveryManager::enterRecoveryModeAll()
{
    lg2::info("Entering recovery mode for all MCU devices...");

    if (mcuDevices.empty())
    {
        lg2::warning(
            "No MCU devices to operate on (mcuDevices is empty). GPIO initialization may have failed.");
        return;
    }

    // Step 1: Set all recovery pins LOW
    for (auto& [deviceId, device] : mcuDevices)
    {
        if (!device.recoveryPin || !device.resetPin)
        {
            lg2::error("Skipping {DEV}: GPIO lines not initialized", "DEV",
                       mcuMap[deviceId].device);
            continue;
        }
        device.recoveryPin.set_value(0);
    }
    usleep(mcuResetActiveUs);

    // Step 2: Set all reset pins LOW
    for (auto& [deviceId, device] : mcuDevices)
    {
        if (!device.recoveryPin || !device.resetPin)
        {
            continue;
        }
        device.resetPin.set_value(0);
    }
    usleep(mcuResetActiveUs);

    // Step 3: Set all reset pins HIGH
    for (auto& [deviceId, device] : mcuDevices)
    {
        if (!device.recoveryPin || !device.resetPin)
        {
            continue;
        }
        device.resetPin.set_value(1);
    }
    sleep(mcuResetDelaySec);
}

void MCURecoveryManager::exitRecoveryMode(const std::string& deviceId)
{
    lg2::info("{DEV} exiting recovery mode...", "DEV", mcuMap[deviceId].device);

    if (!mcuDevices[deviceId].recoveryPin || !mcuDevices[deviceId].resetPin)
    {
        lg2::error(
            "Cannot exit recovery mode for {DEV}: GPIO lines not initialized",
            "DEV", mcuMap[deviceId].device);
        return;
    }
    mcuDevices[deviceId].recoveryPin.set_value(1);
    usleep(mcuResetActiveUs);
    mcuDevices[deviceId].resetPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[deviceId].resetPin.set_value(1);
    sleep(mcuResetDelaySec);
}

void MCURecoveryManager::handleRecoveryError(const std::string& deviceId)
{
    exitRecoveryMode(deviceId);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::MCURecovery,
            deviceRecoveryFailed, mcuMap[deviceId].device);
    }
}

std::string MCURecoveryManager::getFullPortPath(libusb_device* dev)
{
    uint8_t ports[8] = {0};
    int pathLen = libusb_get_port_numbers(dev, ports, sizeof(ports));
    if (pathLen < 0)
    {
        lg2::error("Error getting port numbers: {ERR}", "ERR",
                   libusb_error_name(pathLen));
        return "";
    }

    std::string path = std::to_string(libusb_get_bus_number(dev)) + "-";
    for (int i = 0; i < pathLen; ++i)
    {
        path += std::to_string(ports[i]);
        if (i < pathLen - 1)
        {
            path += ".";
        }
    }
    return path;
}

void MCURecoveryManager::updateDevHealth(const std::string& deviceId,
                                         libusb_config_descriptor* config)
{
    mcuDevices[deviceId].hasMctpClass = false;
    bool hasHidClass = false;
    for (int i = 0; i < config->bNumInterfaces; i++)
    {
        const struct libusb_interface* interface = &config->interface[i];
        // check if the MCU has MCTP class and HID class
        // the MCU is healthy if it has MCTP class
        // the MCU is in recovery mode if it only has HID class
        for (int j = 0; j < interface->num_altsetting; j++)
        {
            const struct libusb_interface_descriptor* altsetting =
                &interface->altsetting[j];
            if (altsetting->bInterfaceClass == LIBUSB_CLASS_MCTP)
            {
                mcuDevices[deviceId].inRecoveryMode = false;
                mcuDevices[deviceId].hasMctpClass = true;
                return;
            }
            else if (altsetting->bInterfaceClass == LIBUSB_CLASS_HID)
            {
                hasHidClass = true;
            }
        }
    }

    // only set it to true if there is a HID class, otherwise it is in an
    // unknown state
    mcuDevices[deviceId].inRecoveryMode = hasHidClass;
}

bool MCURecoveryManager::updateUsbDevInfo(const std::string& deviceId)
{
    const uint8_t maxRetries = 5;
    uint8_t retries = 0;
    const auto& usbPort = mcuMap[deviceId].usbPort;

    while (retries < maxRetries)
    {
        libusb_device** deviceList = nullptr;
        ssize_t deviceCount = libusb_get_device_list(context, &deviceList);
        mcuDevices[deviceId].curUsbDevice = nullptr;

        if (deviceCount < 0)
        {
            lg2::error("Failed to get USB device list: {ERR}", "ERR",
                       libusb_error_name(deviceCount));
            retries++;
            continue;
        }

        bool deviceFound = false;
        for (ssize_t i = 0; i < deviceCount; ++i)
        {
            libusb_device* device = deviceList[i];
            if (usbPort == getFullPortPath(device))
            {
                deviceFound = true;
                mcuDevices[deviceId].curUsbDevice = device;

                int ret = libusb_get_device_descriptor(
                    device, &mcuDevices[deviceId].curUsbDesc);
                if (ret != LIBUSB_SUCCESS)
                {
                    lg2::error("Failed to get descriptor for {PORT}: {ERR}",
                               "PORT", usbPort, "ERR", libusb_error_name(ret));
                    mcuDevices[deviceId].curUsbDevice = nullptr;
                    retries++;
                    break;
                }

                libusb_config_descriptor* config = nullptr;
                ret = libusb_get_active_config_descriptor(device, &config);
                if (ret != LIBUSB_SUCCESS)
                {
                    lg2::error(
                        "Failed to get config descriptor for {PORT}: {ERR}",
                        "PORT", usbPort, "ERR", libusb_error_name(ret));
                    mcuDevices[deviceId].curUsbDevice = nullptr;
                    retries++;
                    break;
                }
                updateDevHealth(deviceId, config);
                libusb_free_config_descriptor(config);
                break;
            }
        }

        libusb_free_device_list(deviceList, 1);

        if (deviceFound && mcuDevices[deviceId].curUsbDevice != nullptr)
        {
            return true;
        }

        retries++;
        if (retries < maxRetries)
        {
            lg2::info(
                "Retrying get USB device info for {PORT} (attempt {CURRENT}/{MAX})",
                "PORT", usbPort, "CURRENT", retries + 1, "MAX", maxRetries);
            sleep(2);
        }
    }

    lg2::error("{DEV} not found on {PORT} after {MAX} attempts", "DEV",
               mcuMap[deviceId].device, "PORT", usbPort, "MAX", maxRetries);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::MCURecovery,
            noDevicesFound, mcuMap[deviceId].device);
    }
    return false;
}

bool MCURecoveryManager::updateDevInfo(const std::string& deviceId)
{
    const auto& info = mcuMap[deviceId];
    if (info.interfaceType == MCUInfo::InterfaceType::I2C)
    {
        return updateI2cDevInfo(deviceId);
    }
    return updateUsbDevInfo(deviceId);
}

void MCURecoveryManager::updateAllDeviceInfo()
{
    for (auto& [deviceId, mcuInfo] : mcuMap)
    {
        if (!updateDevInfo(deviceId))
        {
            continue;
        }
    }
}

void MCURecoveryManager::showAllDeviceStatus()
{
    for (auto& [deviceId, mcuInfo] : mcuMap)
    {
        if (isHealthy(deviceId))
        {
            lg2::info("{DEV} is healthy", "DEV", mcuMap[deviceId].device);
        }
        else if (isInRecoveryMode(deviceId))
        {
            lg2::error("{DEV} is in recovery mode", "DEV",
                       mcuMap[deviceId].device);
        }
        else
        {
            if (mcuInfo.interfaceType == MCUInfo::InterfaceType::I2C)
            {
                lg2::error(
                    "{DEV} is in unknown state: normal I2C address 0x{NORM} recovery I2C address 0x{REC}",
                    "DEV", mcuMap[deviceId].device, "NORM",
                    toHexString(mcuInfo.normalI2cAddress), "REC",
                    toHexString(mcuInfo.recoveryI2cAddress));
            }
            else
            {
                lg2::error(
                    "{DEV} is in unknown state: PID = 0x{PID}", "DEV",
                    mcuMap[deviceId].device, "PID",
                    toHexString(mcuDevices[deviceId].curUsbDesc.idProduct));
            }
        }
    }
}

bool MCURecoveryManager::isEncryptKeyEmpty(const std::string& rawData)
{
    std::string hexData;
    std::istringstream iss(rawData);
    std::string line;
    bool dataStarted = false;

    while (std::getline(iss, line))
    {
        if (line.find("Successful response to command 'read-memory'") !=
            std::string::npos)
        {
            dataStarted = true;
            continue;
        }

        if (dataStarted && hexData.length() < lenEncryptKey)
        {
            // only keep hex characters
            for (char c : line)
            {
                if (std::isxdigit(c))
                {
                    hexData += c;
                }
            }
        }

        if (hexData.length() >= lenEncryptKey)
        {
            break;
        }
    }

    if (hexData.length() != lenEncryptKey)
    {
        lg2::error("Invalid hex data length: {LEN}", "LEN", hexData.length());
        return false;
    }

    // check if all characters are '0' which means key is not set
    return std::all_of(hexData.begin(), hexData.end(),
                       [](char c) { return c == '0'; });
}

bool MCURecoveryManager::isSB3FileValid(const std::string& binaryFilePath)
{
    // first 4 bytes are magic number "sbv3" and last 4 bytes are format
    // version "3.1"
    const unsigned char expectedSb3Hdr[8] = {0x73, 0x62, 0x76, 0x33,
                                             0x01, 0x00, 0x03, 0x00};
    unsigned char hdr[8] = {0};

    std::ifstream file(binaryFilePath, std::ios::binary);
    if (!file.is_open())
    {
        lg2::error("Failed to open {FILE}", "FILE", binaryFilePath);
        return false;
    }

    if (!file.read(reinterpret_cast<char*>(hdr), 8))
    {
        lg2::error("Failed to read {FILE}", "FILE", binaryFilePath);
        return false;
    }

    if (memcmp(hdr, expectedSb3Hdr, 8) != 0)
    {
        // for sb4, the first byte should be 0x02 and the fourth byte
        // should be 0x87
        if (hdr[0] == 0x02 && hdr[3] == 0x87)
        {
            return true;
        }

        lg2::error("SB3/SB4 Header mismatch in {FILE}", "FILE", binaryFilePath);
        return false;
    }
    return true;
}

void MCURecoveryManager::performRecovery(const std::string& deviceId,
                                         const std::string& binaryFilePath)
{
    const auto& info = mcuMap[deviceId];
    std::string blhostTarget;
    if (info.interfaceType == MCUInfo::InterfaceType::USB)
    {
        std::string usbBusDev = std::to_string(getBusNumber(deviceId)) + ":" +
                                std::to_string(getDeviceNumber(deviceId));
        std::string usbVidPid =
            toHexString(mcuDevices[deviceId].curUsbDesc.idVendor) + ":" +
            toHexString(mcuDevices[deviceId].curUsbDesc.idProduct);
        blhostTarget =
            "--usb-bus-device " + usbBusDev + " --usb-id " + usbVidPid;
    }
    else if (info.interfaceType == MCUInfo::InterfaceType::I2C)
    {
        blhostTarget =
            "-i " + getI2cTarget(info.i2cBus, info.recoveryI2cAddress);
    }
    else
    {
        lg2::error("Unknown interface for {DEV}", "DEV", info.device);
        return;
    }

    lg2::info("Performing recovery on {DEV}", "DEV", mcuMap[deviceId].device);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistry(recoveryStarted,
                                               mcuMap[deviceId].device);
    }

    try
    {
        // get security state to check if MCU is locked
        std::string cmdSecState =
            "blhost " + blhostTarget + " get-property security-state";
        lg2::info("execute: {CMD}", "CMD", cmdSecState);
        std::string secStateOutput = executeCommand(cmdSecState);
        if (!isCommandSuccessful(secStateOutput))
        {
            lg2::error("Get security state failed");
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(
                        MCURecoveryErrorCode::GetSecurityStateFailed),
                    mcuMap[deviceId].device);
            }
            handleRecoveryError(deviceId);
            return;
        }

        // check if MCU is locked
        if (secStateOutput.find("UNSECURE") != std::string::npos)
        {
            // UNSECURE - MCU is unlocked / not fully provisioned. Log
            // as warning so the Redfish Task is not flagged Critical.
            lg2::error("{DEV} Security State = UNSECURE", "DEV",
                       mcuMap[deviceId].device);
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(
                        MCURecoveryErrorCode::NotSecureDevice),
                    mcuMap[deviceId].device, Level::Warning);
            }

            lg2::info("Checking if encrypt key is set...");
            // check if encrypt key is set as blhost only receives SB3 file if
            // encrypt key is set
            std::string cmdGetKey =
                "blhost " + blhostTarget + " read-memory 0x1004160 48";
            lg2::info("execute: {CMD}", "CMD", cmdGetKey);
            std::string getKeyOutput = executeCommand(cmdGetKey);
            if (!isCommandSuccessful(getKeyOutput))
            {
                lg2::error("Get encrypt key failed");
                if (messageRegistry)
                {
                    messageRegistry->createMessageRegistryResourceErrors(
                        resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                        static_cast<ErrorCode>(
                            MCURecoveryErrorCode::ReadMemoryFailed),
                        mcuMap[deviceId].device);
                }
                handleRecoveryError(deviceId);
                return;
            }

            if (isEncryptKeyEmpty(getKeyOutput))
            {
                lg2::error("Encrypt key is not set");
                if (messageRegistry)
                {
                    messageRegistry->createMessageRegistryResourceErrors(
                        resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                        static_cast<ErrorCode>(
                            MCURecoveryErrorCode::EncryptKeyNotSet),
                        mcuMap[deviceId].device);
                }
                handleRecoveryError(deviceId);
                return;
            }
        }

        // try receiving SB3 file
        std::string cmdWrite =
            "blhost " + blhostTarget + " receive-sb-file " + binaryFilePath;
        lg2::info("execute: {CMD}", "CMD", cmdWrite);
        std::string write_output = executeCommand(cmdWrite);
        if (!isCommandSuccessful(write_output))
        {
            lg2::error("Write flash failed");
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(MCURecoveryErrorCode::CmdExecFailed),
                    mcuMap[deviceId].device);
            }
            handleRecoveryError(deviceId);
            return;
        }

        // exit recovery mode
        exitRecoveryMode(deviceId);
        if (!updateDevInfo(deviceId))
        {
            handleRecoveryError(deviceId);
            return;
        }

        if (isHealthy(deviceId))
        {
            if (mcuMap[deviceId].interfaceType == MCUInfo::InterfaceType::I2C)
            {
                lg2::info(
                    "{DEV} successfully recovered, normal I2C address 0x{ADDR} detected",
                    "DEV", mcuMap[deviceId].device, "ADDR",
                    toHexString(mcuMap[deviceId].normalI2cAddress));
            }
            else
            {
                lg2::info(
                    "{DEV} successfully recovered, PID = 0x{PID} as expected",
                    "DEV", mcuMap[deviceId].device, "PID",
                    toHexString(mcuDevices[deviceId].curUsbDesc.idProduct));
            }
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistry(recoverySuccessful,
                                                       mcuMap[deviceId].device);
            }
        }
        else
        {
            if (mcuMap[deviceId].interfaceType == MCUInfo::InterfaceType::I2C)
            {
                lg2::error(
                    "{DEV} Recovery failed, normal I2C address 0x{ADDR} not detected",
                    "DEV", mcuMap[deviceId].device, "ADDR",
                    toHexString(mcuMap[deviceId].normalI2cAddress));
            }
            else
            {
                lg2::error(
                    "{DEV} Recovery failed, PID = 0x{ACTUAL} does not match expected 0x{EXPECTED}",
                    "DEV", mcuMap[deviceId].device, "ACTUAL",
                    toHexString(mcuDevices[deviceId].curUsbDesc.idProduct),
                    "EXPECTED", toHexString(mcuMap[deviceId].functionalPid));
            }
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    deviceRecoveryFailed, mcuMap[deviceId].device);
            }
        }
        return;
    }
    catch (const std::exception& e)
    {
        lg2::error("command execution failed: {ERR}", "ERR", e.what());
        if (messageRegistry)
        {
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                static_cast<ErrorCode>(MCURecoveryErrorCode::CmdExecFailed),
                mcuMap[deviceId].device);
        }
    }
}

void MCURecoveryManager::performRecoveryFlow(const std::string& binaryFilePath,
                                             bool forceUpdate)
{
    lg2::info("Starting MCU Recovery flow...");

    if (!isSB3FileValid(binaryFilePath))
    {
        if (messageRegistry)
        {
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                static_cast<ErrorCode>(MCURecoveryErrorCode::InvalidSBFile),
                "Invalid SB3/SB4 file");
        }
        return;
    }

    // Go through all MCUs and perform recovery if needed
    for (const auto& [deviceId, mcuInfo] : mcuMap)
    {
        if (!updateDevInfo(deviceId))
        {
            continue;
        }

        if (!isDeviceProvisioned(deviceId))
        {
            lg2::error("Non-provisioned device detected, skipping recovery",
                       "DEV", mcuInfo.device);
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(
                        MCURecoveryErrorCode::DevNotProvisioned),
                    mcuMap[deviceId].device);
            }
            continue;
        }

        if (isHealthy(deviceId))
        {
            if (forceUpdate)
            {
                lg2::info(
                    "{DEV} is healthy, but forceUpdate is set, performing recovery",
                    "DEV", mcuMap[deviceId].device);
                enterRecoveryMode(deviceId);
                if (!updateDevInfo(deviceId))
                {
                    continue;
                }
                performRecovery(deviceId, binaryFilePath);
            }
            else
            {
                lg2::info("{DEVICE} is healthy", "DEVICE",
                          mcuMap[deviceId].device);
                if (messageRegistry)
                {
                    messageRegistry->createMessageRegistry(
                        firmwareNotInRecovery, mcuMap[deviceId].device);
                }
            }
        }
        else
        {
            lg2::error("{DEV} is not healthy, performing recovery", "DEV",
                       mcuMap[deviceId].device);

            // Put the MCU into force recovery mode to avoid the NXP known
            // issue (TRNG issue) where MCU cannot receive the SB3 file even
            // though the MCU is in ISP mode
            enterRecoveryMode(deviceId);
            if (!updateDevInfo(deviceId))
            {
                continue;
            }
            performRecovery(deviceId, binaryFilePath);
        }
    }

    lg2::info("MCU Recovery flow completed");
}

void MCURecoveryManager::performResetFlow()
{
    // Copy the map to a list for easy removal of elements
    std::list<std::pair<std::string, MCUInfo>> mcuList(mcuMap.begin(),
                                                       mcuMap.end());

    for (int i = 0; i < 10 && !mcuList.empty(); ++i)
    {
        // Put all MCUs into reset state (only for valid GPIO lines)
        for (const auto& [deviceId, device] : mcuDevices)
        {
            if (!device.resetPin)
            {
                continue;
            }
            device.resetPin.set_value(0);
        }
        usleep(mcuResetActiveUs);

        // Release all MCU reset pins (only for valid GPIO lines)
        for (const auto& [deviceId, device] : mcuDevices)
        {
            if (!device.resetPin)
            {
                continue;
            }
            device.resetPin.set_value(1);
        }
        sleep(mcuResetDelaySec);

        for (auto it = mcuList.begin(); it != mcuList.end();)
        {
            const auto& [deviceId, mcuInfo] = *it;
            if (updateDevInfo(deviceId))
            {
                if (isHealthy(deviceId))
                {
                    lg2::info("{DEV} is healthy", "DEV",
                              mcuMap[deviceId].device);
                    // Remove MCU from list if MCU is healthy
                    it = mcuList.erase(it);
                    // Release and remove the GPIO line to prevent
                    // unnecessary reset (only if lines were initialized)
                    if (mcuDevices[deviceId].resetPin)
                    {
                        mcuDevices[deviceId].resetPin.release();
                        mcuDevices[deviceId].resetPin = {};
                    }
                    if (mcuDevices[deviceId].recoveryPin)
                    {
                        mcuDevices[deviceId].recoveryPin.release();
                        mcuDevices[deviceId].recoveryPin = {};
                    }
                    continue;
                }
                else
                {
                    if (mcuInfo.interfaceType == MCUInfo::InterfaceType::I2C)
                    {
                        lg2::error(
                            "{DEV} is not healthy, retrying. Normal I2C 0x{NORM} Recovery I2C 0x{REC}",
                            "DEV", mcuMap[deviceId].device, "NORM",
                            toHexString(mcuInfo.normalI2cAddress), "REC",
                            toHexString(mcuInfo.recoveryI2cAddress));
                    }
                    else
                    {
                        lg2::error(
                            "{DEV} is not healthy, retrying. Current PID = 0x{PID} (expected: 0x{EXPECTED})",
                            "DEV", mcuMap[deviceId].device, "PID",
                            toHexString(
                                mcuDevices[deviceId].curUsbDesc.idProduct),
                            "EXPECTED", toHexString(mcuInfo.functionalPid));
                    }
                }
            }
            ++it;
        }
    }

    int failedDevices = mcuList.size();
    if (failedDevices == 0)
    {
        lg2::info("All MCUs are healthy.");
    }
    else
    {
        lg2::error("Some MCUs failed to become healthy after 10 attempts:");
        for (const auto& [deviceId, mcuInfo] : mcuList)
        {
            lg2::error(" - {DEV} on {PORT}", "DEV", mcuMap[deviceId].device,
                       "PORT", mcuInfo.usbPort);
        }
    }

    // Release all GPIO lines (only for lines that were actually requested)
    for (auto& [deviceId, device] : mcuDevices)
    {
        if (device.resetPin)
        {
            device.resetPin.release();
            device.resetPin = {};
        }
        if (device.recoveryPin)
        {
            device.recoveryPin.release();
            device.recoveryPin = {};
        }
    }
}

} // namespace mcu_recovery_manager
