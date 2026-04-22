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

#include <cstdint>
#include <string>

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
 * This structure contains information about the MCU device's interface
 * (USB or I2C), connection details, GPIO names for reset and recovery,
 * device identifiers, and the corresponding product IDs for functionality.
 */
struct MCUInfo
{
    std::string device;
    std::string usbPort;
    std::string resetGpioName;
    std::string recoveryGpioName;
    uint8_t i2cBus = 0;
    uint16_t normalI2cAddress = 0;
    uint16_t recoveryI2cAddress = 0;
    uint16_t functionalPid = 0;
    enum class InterfaceType
    {
        USB,
        I2C,
        Unknown
    } interfaceType = InterfaceType::Unknown;
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
    libusb_device* curUsbDevice = nullptr;
    libusb_device_descriptor curUsbDesc{};
    gpiod::line resetPin{};
    gpiod::line recoveryPin{};
    bool inRecoveryMode = false;
    bool hasMctpClass = false;
    bool i2cHealthy = false;
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
     * @param deviceId The MCU device identifier from configuration.
     * @param binaryFilePath The path to the binary file to be used for
     * recovery.
     */
    void performRecovery(const std::string& deviceId,
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
     * @brief Updates the status of a specific MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the update is successful, false otherwise.
     */
    bool updateDevInfo(const std::string& deviceId);

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
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the MCU device is healthy, false otherwise.
     */
    bool isHealthy(const std::string& deviceId)
    {
        try
        {
            if (mcuMap.at(deviceId).interfaceType ==
                MCUInfo::InterfaceType::I2C)
            {
                return mcuDevices.at(deviceId).i2cHealthy;
            }
            return (mcuDevices.at(deviceId).hasMctpClass ||
                    (mcuMap.at(deviceId).functionalPid ==
                     mcuDevices.at(deviceId).curUsbDesc.idProduct));
        }
        catch (const std::out_of_range& oor)
        {
            lg2::error(
                "Device '{ID}' not found in mcuMap or mcuDevices. Error: {ERROR}",
                "ID", deviceId, "ERROR", oor.what());
            return false;
        }
    }

    /**
     * @brief Checks if the MCU device is in recovery mode.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the MCU device is in recovery mode, false otherwise.
     */
    bool isInRecoveryMode(const std::string& deviceId)
    {
        try
        {
            return mcuDevices.at(deviceId).inRecoveryMode;
        }
        catch (const std::out_of_range& oor)
        {
            lg2::error("Device '{ID}' not found in mcuDevices. Error: {ERROR}",
                       "ID", deviceId, "ERROR", oor.what());
            return false;
        }
    }

    /**
     * @brief Enters the recovery mode for a specific MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     */
    void enterRecoveryMode(const std::string& deviceId);

    /**
     * @brief Enters the recovery mode for all MCU devices at once.
     * This is more efficient than calling enterRecoveryMode() for each device
     * individually, as it applies GPIO changes to all devices in coordinated
     * phases (recovery pins low, reset pins low, reset pins high).
     */
    void enterRecoveryModeAll();

    /**
     * @brief Exits the recovery mode for a specific MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     */
    void exitRecoveryMode(const std::string& deviceId);

    /**
     * @brief Initializes the GPIO lines for the MCU devices.
     *
     * @return True if the initialization is successful, false otherwise.
     */
    bool initGpioLines();

    /**
     * @brief Releases GPIO lines for all MCU devices.
     *
     * This should be called after recovery operations to free GPIO resources.
     */
    void releaseGpioLines();

  private:
    libusb_context* context;
    std::map<std::string, MCUInfo> mcuMap;
    std::map<std::string, MCUDevice> mcuDevices;
    std::unique_ptr<MessageRegistry> messageRegistry;

    /**
     * @brief Gets the full port path for a specific MCU device.
     *
     * @param dev The libusb device pointer.
     * @return The full port path for the MCU device.
     */
    std::string getFullPortPath(libusb_device* dev);

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
     * @param deviceId The MCU device identifier from configuration.
     * @param config The libusb config descriptor.
     */
    void updateDevHealth(const std::string& deviceId,
                         libusb_config_descriptor* config);

    /**
     * @brief Checks if the MCU device is provisioned.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the device is provisioned, false otherwise.
     */
    bool isDeviceProvisioned(const std::string& deviceId)
    {
        const auto& info = mcuMap[deviceId];
        if (info.interfaceType == MCUInfo::InterfaceType::I2C)
        {
            return true;
        }
        return nvdaVendorId == mcuDevices[deviceId].curUsbDesc.idVendor;
    }

    /**
     * @brief Gets the USB bus number of the MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return The USB bus number of the MCU device.
     */
    uint8_t getBusNumber(const std::string& deviceId)
    {
        return libusb_get_bus_number(mcuDevices[deviceId].curUsbDevice);
    }

    /**
     * @brief Gets the USB device address of the MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return The USB device address of the MCU device.
     */
    uint8_t getDeviceNumber(const std::string& deviceId)
    {
        return libusb_get_device_address(mcuDevices[deviceId].curUsbDevice);
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
     * @param deviceId The MCU device identifier from configuration.
     */
    void handleRecoveryError(const std::string& deviceId);

    /**
     * @brief Updates the status of a specific USB MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the update is successful, false otherwise.
     */
    bool updateUsbDevInfo(const std::string& deviceId);

    /**
     * @brief Updates the status of a specific I2C MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @return True if the update is successful, false otherwise.
     */
    bool updateI2cDevInfo(const std::string& deviceId);

    /**
     * @brief Probes the I2C address of a specific MCU device.
     *
     * @param deviceId The MCU device identifier from configuration.
     * @param address The I2C address to probe.
     * @return True if the probe is successful, false otherwise.
     */
    bool probeI2cAddress(const std::string& deviceId, uint16_t address);

    /**
     * @brief Gets the I2C target string for a specific MCU device.
     *
     * @param bus The I2C bus number.
     * @param address The I2C address.
     * @return The I2C target string.
     */
    std::string getI2cTarget(uint8_t bus, uint16_t address);
};

} // namespace mcu_recovery_manager