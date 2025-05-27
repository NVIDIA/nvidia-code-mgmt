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

#pragma once

#include "message_registry.hpp"

#include <libusb-1.0/libusb.h>

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>

namespace mcu_recovery_manager
{

// key is 48 bytes so it should be 96 characters
constexpr uint8_t lenEncryptKey = 96;
constexpr uint16_t nvdaVendorId = 0x0955;
constexpr uint32_t mcuResetActiveUs = 500000;
constexpr uint32_t mcuResetDelaySec = 3;

constexpr uint8_t LIBUSB_CLASS_MCTP = 0x14;

/**
 * @struct MCUInfo
 * @brief Represents a MCU device and its USB configuration.
 *
 * This structure contains information about the MCU device's USB port,
 * GPIO names for reset and recovery, device identifiers, and the
 * corresponding product IDs for recovery and functionality.
 */
struct MCUInfo
{
    std::string usbPort;
    std::string resetGpioName;
    std::string recoveryGpioName;
    std::string device;
    uint16_t functionalPid;
};

/**
 * @struct MCUDevice
 * @brief Represents a MCU device and its initialized variables and operational
 * status.
 *
 * This structure holds the current USB device pointer, its descriptor,
 * GPIO lines for reset and recovery, and the recovery mode status.
 */
struct MCUDevice
{
    libusb_device* curUsbDevice;
    libusb_device_descriptor curUsbDesc;
    gpiod::line resetPin;
    gpiod::line recoveryPin;
    bool inRecoveryMode;
    bool hasMctpClass;
};

/**
 * @class MCURecoveryManager
 * @brief Manages the recovery process of MCU devices.
 *
 * This class is responsible for recovering, resetting, and reporting
 * the status of MCU devices.
 * It interacts with libusb for USB device communication and maintains
 * the status of each MCU device.
 */
class MCURecoveryManager
{
  public:
    MCURecoveryManager();
    ~MCURecoveryManager();

    /**
     * @brief Initializes the recovery manager with a map of MCU devices and a
     * message registry.
     *
     * @param mcuMap A map of MCU devices with their corresponding information.
     * @param messageRegistry A pointer to a message registry.
     * @param initGpio Initialize GPIO lines.
     * @return True if the initialization is successful, false otherwise.
     */
    bool initialize(const std::map<std::string, MCUInfo>& mcuMap,
                    std::unique_ptr<MessageRegistry> messageRegistry = nullptr,
                    bool initGpio = false);

    /**
     * @brief Performs a recovery flow on a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     * @param binaryFilePath The path to the binary file to be used for
     * recovery.
     */
    void performRecovery(const std::string& usbPort,
                         const std::string& binaryFilePath);

    /**
     * @brief Performs a recovery flow on all MCU devices.
     *
     * @param binaryFilePath The path to the binary file to be used for
     * recovery.
     * @param forceUpdate Force the recovery flow to update the MCU device.
     */
    void performRecoveryFlow(const std::string& binaryFilePath,
                             bool forceUpdate);

    /**
     * @brief Performs a reset flow on all MCU devices.
     */
    void performResetFlow();

    /**
     * @brief Updates the status of all MCU devices.
     */
    void updateAllDeviceInfo();

    /**
     * @brief Shows the status of all MCU devices.
     */
    void showAllDeviceStatus();

    /**
     * @brief Checks if the MCU device is healthy.
     *
     * For general case, we can determine the device is healthy if the
     * functional PID matches the it's current product ID. However, for
     * Iris MCU (PX86E) it's different as Iris MCU PIDs are the same when
     * it's in ISP mode. So we need to check if the device has MCTP class.
     * the MCU is healthy if it has MCTP class.
     *
     * @param usbPort The USB port of the MCU device.
     * @return True if the MCU device is healthy, false otherwise.
     */
    bool isHealthy(const std::string& usbPort)
    {
        try
        {
            return (mcuDevices.at(usbPort).hasMctpClass ||
                    (mcuMap.at(usbPort).functionalPid ==
                     mcuDevices.at(usbPort).curUsbDesc.idProduct));
        }
        catch (const std::out_of_range& oor)
        {
            lg2::error(
                "USB port '{PORT}' not found in mcuMap or mcuDevices. Error: {ERROR}",
                "PORT", usbPort, "ERROR", oor.what());
            return false;
        }
    }

    /**
     * @brief Checks if the MCU device is in recovery mode.
     *
     * @param usbPort The USB port of the MCU device.
     * @return True if the MCU device is in recovery mode, false otherwise.
     */
    bool isInRecoveryMode(const std::string& usbPort)
    {
        try
        {
            return mcuDevices.at(usbPort).inRecoveryMode;
        }
        catch (const std::out_of_range& oor)
        {
            lg2::error(
                "USB port '{PORT}' not found in mcuDevices. Error: {ERROR}",
                "PORT", usbPort, "ERROR", oor.what());
            return false;
        }
    }

  private:
    libusb_context* context;
    std::map<std::string, MCUInfo> mcuMap;
    std::map<std::string, MCUDevice> mcuDevices;
    std::unique_ptr<MessageRegistry> messageRegistry;

    /**
     * @brief Initializes the GPIO lines for the MCU devices.
     *
     * @return True if the initialization is successful, false otherwise.
     */
    bool initGpioLines();

    /**
     * @brief Enters the recovery mode for a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     */
    void enterRecoveryMode(const std::string& usbPort);

    /**
     * @brief Exits the recovery mode for a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     */
    void exitRecoveryMode(const std::string& usbPort);

    /**
     * @brief Gets the full port path for a specific MCU device.
     *
     * @param dev The libusb device pointer.
     * @return The full port path for the MCU device.
     */
    std::string getFullPortPath(libusb_device* dev);

    /**
     * @brief Updates the status of a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     * @return True if the update is successful, false otherwise.
     */
    bool updateDevInfo(const std::string& usbPort);

    /**
     * @brief Checks if the encrypt key is empty.
     *
     * @param rawData The output of the blhost command.
     * @return True if the encrypt key is empty, false otherwise.
     */
    bool isEncryptKeyEmpty(const std::string& rawData);

    /**
     * @brief Updates the health of a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     * @param config The libusb config descriptor.
     */
    void updateDevHealth(const std::string& usbPort,
                         libusb_config_descriptor* config);

    /**
     * @brief Checks if the MCU device is provisioned.
     *
     * @param usbPort The USB port of the MCU device.
     * @return True if the device is provisioned, false otherwise.
     */
    bool isDeviceProvisioned(const std::string& usbPort)
    {
        return nvdaVendorId == mcuDevices[usbPort].curUsbDesc.idVendor;
    }

    /**
     * @brief Gets the USB bus number of the MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     * @return The USB bus number of the MCU device.
     */
    uint8_t getBusNumber(const std::string& usbPort)
    {
        return libusb_get_bus_number(mcuDevices[usbPort].curUsbDevice);
    }

    /**
     * @brief Gets the USB device address of the MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     * @return The USB device address of the MCU device.
     */
    uint8_t getDeviceNumber(const std::string& usbPort)
    {
        return libusb_get_device_address(mcuDevices[usbPort].curUsbDevice);
    }

    /**
     * @brief Checks if the SB3 file is valid.
     *
     * @param binaryFilePath The path to the binary file to be used for
     * recovery.
     * @return True if the SB3 file is valid, false otherwise.
     */
    bool isSB3FileValid(const std::string& binaryFilePath);

    /**
     * @brief Handles the recovery error for a specific MCU device.
     *
     * @param usbPort The USB port of the MCU device.
     */
    void handleRecoveryError(const std::string& usbPort);
};

} // namespace mcu_recovery_manager