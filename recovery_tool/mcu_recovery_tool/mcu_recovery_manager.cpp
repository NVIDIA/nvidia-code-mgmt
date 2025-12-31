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

#include <unistd.h>

#include <CLI/CLI.hpp>

#include <list>

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

    if (libusb_init(&context) < 0)
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

bool MCURecoveryManager::initGpioLines()
{
    for (auto& [usbPort, mcuInfo] : mcuMap)
    {
        try
        {
            mcuDevices[usbPort].resetPin =
                gpiod::find_line(mcuInfo.resetGpioName);
            if (!mcuDevices[usbPort].resetPin)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           mcuInfo.resetGpioName);
                continue;
            }
            mcuDevices[usbPort].recoveryPin =
                gpiod::find_line(mcuInfo.recoveryGpioName);
            if (!mcuDevices[usbPort].recoveryPin)
            {
                lg2::error("GPIO line not found: {NAME}", "NAME",
                           mcuInfo.recoveryGpioName);
                continue;
            }
            // set the default value of the reset pin to 1
            mcuDevices[usbPort].resetPin.request(
                {"mcu_recovery", gpiod::line_request::DIRECTION_OUTPUT, 0}, 1);
            mcuDevices[usbPort].recoveryPin.request(
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

void MCURecoveryManager::enterRecoveryMode(const std::string& usbPort)
{
    lg2::info("{DEV} entering recovery mode...", "DEV", mcuMap[usbPort].device);
    mcuDevices[usbPort].recoveryPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[usbPort].resetPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[usbPort].resetPin.set_value(1);
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
    for (auto& [usbPort, device] : mcuDevices)
    {
        device.recoveryPin.set_value(0);
    }
    usleep(mcuResetActiveUs);

    // Step 2: Set all reset pins LOW
    for (auto& [usbPort, device] : mcuDevices)
    {
        device.resetPin.set_value(0);
    }
    usleep(mcuResetActiveUs);

    // Step 3: Set all reset pins HIGH
    for (auto& [usbPort, device] : mcuDevices)
    {
        device.resetPin.set_value(1);
    }
    sleep(mcuResetDelaySec);
}

void MCURecoveryManager::exitRecoveryMode(const std::string& usbPort)
{
    lg2::info("{DEV} exiting recovery mode...", "DEV", mcuMap[usbPort].device);

    mcuDevices[usbPort].recoveryPin.set_value(1);
    usleep(mcuResetActiveUs);
    mcuDevices[usbPort].resetPin.set_value(0);
    usleep(mcuResetActiveUs);
    mcuDevices[usbPort].resetPin.set_value(1);
    sleep(mcuResetDelaySec);
}

void MCURecoveryManager::handleRecoveryError(const std::string& usbPort)
{
    exitRecoveryMode(usbPort);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::MCURecovery,
            deviceRecoveryFailed, mcuMap[usbPort].device);
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

void MCURecoveryManager::updateDevHealth(const std::string& usbPort,
                                         libusb_config_descriptor* config)
{
    mcuDevices[usbPort].hasMctpClass = false;
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
                mcuDevices[usbPort].inRecoveryMode = false;
                mcuDevices[usbPort].hasMctpClass = true;
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
    mcuDevices[usbPort].inRecoveryMode = hasHidClass;
}

bool MCURecoveryManager::updateDevInfo(const std::string& usbPort)
{
    const uint8_t maxRetries = 5;
    uint8_t retries = 0;

    while (retries < maxRetries)
    {
        libusb_device** deviceList = nullptr;
        ssize_t deviceCount = libusb_get_device_list(context, &deviceList);
        mcuDevices[usbPort].curUsbDevice = nullptr;

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
                mcuDevices[usbPort].curUsbDevice = device;

                int ret = libusb_get_device_descriptor(
                    device, &mcuDevices[usbPort].curUsbDesc);
                if (ret != LIBUSB_SUCCESS)
                {
                    lg2::error("Failed to get descriptor for {PORT}: {ERR}",
                               "PORT", usbPort, "ERR", libusb_error_name(ret));
                    mcuDevices[usbPort].curUsbDevice = nullptr;
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
                    mcuDevices[usbPort].curUsbDevice = nullptr;
                    retries++;
                    break;
                }
                updateDevHealth(usbPort, config);
                libusb_free_config_descriptor(config);
                break;
            }
        }

        libusb_free_device_list(deviceList, 1);

        if (deviceFound && mcuDevices[usbPort].curUsbDevice != nullptr)
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
               mcuMap[usbPort].device, "PORT", usbPort, "MAX", maxRetries);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::MCURecovery,
            noDevicesFound, mcuMap[usbPort].device);
    }
    return false;
}

void MCURecoveryManager::updateAllDeviceInfo()
{
    for (auto& [usbPort, mcuInfo] : mcuMap)
    {
        updateDevInfo(usbPort);
    }
}

void MCURecoveryManager::showAllDeviceStatus()
{
    for (auto& [usbPort, mcuInfo] : mcuMap)
    {
        if (isHealthy(usbPort))
        {
            lg2::info("{DEV} is healthy", "DEV", mcuMap[usbPort].device);
        }
        else if (isInRecoveryMode(usbPort))
        {
            lg2::error("{DEV} is in recovery mode", "DEV",
                       mcuMap[usbPort].device);
        }
        else
        {
            lg2::error("{DEV} is in unknown state: PID = 0x{PID}", "DEV",
                       mcuMap[usbPort].device, "PID",
                       toHexString(mcuDevices[usbPort].curUsbDesc.idProduct));
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
    const unsigned char expectedHdr[8] = {0x73, 0x62, 0x76, 0x33,
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

    if (memcmp(hdr, expectedHdr, 8) != 0)
    {
        lg2::error("Header mismatch in {FILE}", "FILE", binaryFilePath);
        return false;
    }
    return true;
}

void MCURecoveryManager::performRecovery(const std::string& usbPort,
                                         const std::string& binaryFilePath)
{
    std::string usbBusDev = std::to_string(getBusNumber(usbPort)) + ":" +
                            std::to_string(getDeviceNumber(usbPort));
    std::string usbVidPid =
        toHexString(mcuDevices[usbPort].curUsbDesc.idVendor) + ":" +
        toHexString(mcuDevices[usbPort].curUsbDesc.idProduct);

    lg2::info("Performing recovery on {DEV}", "DEV", mcuMap[usbPort].device);
    if (messageRegistry)
    {
        messageRegistry->createMessageRegistry(recoveryStarted,
                                               mcuMap[usbPort].device);
    }

    try
    {
        // get security state to check if MCU is locked
        std::string cmdSecState = "blhost --usb-bus-device " + usbBusDev +
                                  " --usb-id " + usbVidPid +
                                  " get-property security-state";
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
                    mcuMap[usbPort].device);
            }
            handleRecoveryError(usbPort);
            return;
        }

        // check if MCU is locked
        if (secStateOutput.find("UNSECURE") != std::string::npos)
        {
            lg2::error("{DEV} Security State = UNSECURE", "DEV",
                       mcuMap[usbPort].device);
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(
                        MCURecoveryErrorCode::NotSecureDevice),
                    mcuMap[usbPort].device);
            }

            lg2::info("Checking if encrypt key is set...");
            // check if encrypt key is set as blhost only receives SB3 file if
            // encrypt key is set
            std::string cmdGetKey = "blhost --usb-bus-device " + usbBusDev +
                                    " --usb-id " + usbVidPid +
                                    " read-memory 0x1004160 48";
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
                        mcuMap[usbPort].device);
                }
                handleRecoveryError(usbPort);
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
                        mcuMap[usbPort].device);
                }
                handleRecoveryError(usbPort);
                return;
            }
        }

        // try receiving SB3 file
        std::string cmdWrite = "blhost --usb-bus-device " + usbBusDev +
                               " --usb-id " + usbVidPid + " receive-sb-file " +
                               binaryFilePath;
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
                    mcuMap[usbPort].device);
            }
            handleRecoveryError(usbPort);
            return;
        }

        // exit recovery mode
        exitRecoveryMode(usbPort);
        if (!updateDevInfo(usbPort))
        {
            handleRecoveryError(usbPort);
            return;
        }

        if (isHealthy(usbPort))
        {
            lg2::info("{DEV} successfully recovered, PID = 0x{PID} as expected",
                      "DEV", mcuMap[usbPort].device, "PID",
                      toHexString(mcuDevices[usbPort].curUsbDesc.idProduct));
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistry(recoverySuccessful,
                                                       mcuMap[usbPort].device);
            }
        }
        else
        {
            lg2::error(
                "{DEV} Recovery failed, PID = 0x{ACTUAL} does not match expected 0x{EXPECTED}",
                "DEV", mcuMap[usbPort].device, "ACTUAL",
                toHexString(mcuDevices[usbPort].curUsbDesc.idProduct),
                "EXPECTED", toHexString(mcuMap[usbPort].functionalPid));
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    deviceRecoveryFailed, mcuMap[usbPort].device);
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
                mcuMap[usbPort].device);
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
                static_cast<ErrorCode>(MCURecoveryErrorCode::InvalidSB3File),
                "Invalid SB3 file");
        }
        return;
    }

    // Go through all MCUs and perform recovery if needed
    for (const auto& [usbPort, mcuInfo] : mcuMap)
    {
        if (!updateDevInfo(usbPort))
        {
            continue;
        }

        if (!isDeviceProvisioned(usbPort))
        {
            lg2::error(
                "Non-provisioned device detected on {PORT}! (PID: 0x{PID}), skipping recovery",
                "PORT", usbPort, "PID",
                toHexString(mcuDevices[usbPort].curUsbDesc.idProduct));
            if (messageRegistry)
            {
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    static_cast<ErrorCode>(
                        MCURecoveryErrorCode::DevNotProvisioned),
                    mcuMap[usbPort].device);
            }
            continue;
        }

        if (isHealthy(usbPort))
        {
            if (forceUpdate)
            {
                lg2::info(
                    "{DEV} is healthy, but forceUpdate is set, performing recovery",
                    "DEV", mcuMap[usbPort].device);
                enterRecoveryMode(usbPort);
                if (!updateDevInfo(usbPort))
                {
                    continue;
                }
                performRecovery(usbPort, binaryFilePath);
            }
            else
            {
                lg2::info("{DEVICE} is healthy", "DEVICE",
                          mcuMap[usbPort].device);
                if (messageRegistry)
                {
                    messageRegistry->createMessageRegistry(
                        firmwareNotInRecovery, mcuMap[usbPort].device);
                }
            }
        }
        else
        {
            lg2::error("{DEV} is not healthy, performing recovery", "DEV",
                       mcuMap[usbPort].device);

            // Put the MCU into force recovery mode to avoid the NXP known
            // issue (TRNG issue) where MCU cannot receive the SB3 file even
            // though the MCU is in ISP mode
            enterRecoveryMode(usbPort);
            if (!updateDevInfo(usbPort))
            {
                continue;
            }
            performRecovery(usbPort, binaryFilePath);
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
        // Put all MCUs into reset state
        for (const auto& [usbPort, device] : mcuDevices)
        {
            device.resetPin.set_value(0);
        }
        usleep(mcuResetActiveUs);

        // Release all MCU reset pins
        for (const auto& [usbPort, device] : mcuDevices)
        {
            device.resetPin.set_value(1);
        }
        sleep(mcuResetDelaySec);

        for (auto it = mcuList.begin(); it != mcuList.end();)
        {
            const auto& [usbPort, mcuInfo] = *it;
            if (updateDevInfo(usbPort))
            {
                if (isHealthy(usbPort))
                {
                    lg2::info("{DEV} is healthy", "DEV",
                              mcuMap[usbPort].device);
                    // Remove MCU from list if MCU is healthy
                    it = mcuList.erase(it);
                    // Release and remove the GPIO line to prevent
                    // unnecessary reset
                    mcuDevices[usbPort].resetPin.release();
                    mcuDevices[usbPort].recoveryPin.release();
                    continue;
                }
                else
                {
                    lg2::error(
                        "{DEV} is not healthy, retrying. Current PID = 0x{PID} (expected: 0x{EXPECTED})",
                        "DEV", mcuMap[usbPort].device, "PID",
                        toHexString(mcuDevices[usbPort].curUsbDesc.idProduct),
                        "EXPECTED", toHexString(mcuInfo.functionalPid));
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
        for (const auto& [usbPort, mcuInfo] : mcuList)
        {
            lg2::error(" - {DEV} on {PORT}", "DEV", mcuMap[usbPort].device,
                       "PORT", usbPort);
        }
    }

    // Release all GPIO lines
    for (const auto& [usbPort, device] : mcuDevices)
    {
        device.resetPin.release();
        device.recoveryPin.release();
    }
}

} // namespace mcu_recovery_manager
