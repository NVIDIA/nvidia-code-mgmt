/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "i2c_utils.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace recovery_tool
{

namespace recovery_commands
{

static constexpr uint8_t indirectStatusExpectedAck = 0x1;
constexpr bool Tx = true;
constexpr bool Rx = false;
static constexpr uint8_t delay1sec = 1;
/**
 * @enum RecoveryCommands
 * @brief Enumerates commands for OCP recovery.
 */
enum class RecoveryCommands : uint8_t
{
    DeviceStatus = 0x24,
    RecoveryCtrl = 0x26,
    RecoveryStatus = 0x27,
    IndirectCtrl = 0x29,
    IndirectStatus = 0x2A,
    IndirectData = 0x2B,
};

/**
 * @enum ResponseLength
 * @brief Enumerates the expected response lengths for various recovery
 * commands.
 */
enum class ResponseLength : uint8_t
{
    DeviceStatusResLen = 25,
    RecoveryStatusResLen = 3,
    IndirectStatusResLen = 7,
};

/**
 * @enum ImageType
 * @brief Enumerates types of images used in OCP recovery.
 */
enum class ImageType : uint8_t
{
    CMS0 = 0,
    CMS1 = 1,
};

/**
 * @enum RecoveryImageSelection
 * @brief Enumerates available sources for selecting recovery images.
 */
enum class RecoveryImageSelection : uint8_t
{
    NoOperation = 0x0,
    FromMemoryWindow = 0x1, // Use Recovery Image from memory window (CMS)
    StoredOnDevice = 0x2,   // Use Recovery Image stored on device (C image)
    // Values from 0x3 to 0xFF are reserved
};

/**
 * @enum ActivateRecoveryImage
 * @brief Enumerates commands for recovery image activation.
 */
enum class ActivateRecoveryImage : uint8_t
{
    DoNotActivate = 0x0, // Do not activate recovery image
    // Values from 0x1 to 0xE are reserved
    ActivateImage = 0xF // Activate recovery image
    // Values from 0x10 to 0xFF are reserved
};
/**
 * @class OCPRecoveryCommands
 * @brief Utility for executing recovery commands on OCP devices.
 */
class OCPRecoveryCommands
{
  private:
    int busAddress;
    int slaveAddress;
    bool verbose;
    bool emulation;
    int i2cFile;

    /**
     * @brief Constructs the path to the I2C device.
     * @return The device path as a string.
     */
    std::string constructI2CDevicePath();

    /**
     * @brief Opens the I2C device for communication.
     */
    void openI2CDevice();

    /**
     * @brief Sets the control register for recovery based on the given image
     * type.
     * @param imageType The type of image for recovery.
     * @param activation Flag indicating activation of the image.
     * @return true if successful, false otherwise.
     */
    bool setRecoveryControlRegisterCommand(ImageType imageType,
                                           bool activation);

    /**
     * @brief Sets the indirect control register based on the given image type.
     * @param imageType The type of image.
     * @return true if successful, false otherwise.
     */
    bool setIndirectControlRegisterCommand(ImageType imageType);

    /**
     * @brief Writes indirect data to the device.
     * @param data A vector containing the chunk of recovery image data to be sent.
     * @return true if successful, false otherwise.
     */
    bool setIndirectDataCommand(const std::vector<uint8_t>& data);

    /**
     * @brief Writes the recovery image to the device.
     * @param imageName The type of image.
     * @param imageData The data of the recovery image.
     * @return true if successful, false otherwise.
     */
    bool writeRecoveryImage(const std::string& imageName,
                            const std::vector<uint8_t>& imageData);

    /**
     * @brief Reads the firmware image from a file.
     * @param filePath Path to the firmware image file.
     * @return A vector containing the image data.
     */
    std::vector<uint8_t> readFirmwareImage(const std::string& filePath);

    /**
     * @brief Retrieves the indirect status of the device.
     * @return A tuple containing success flag, data read from the device
     * and an error message if any.
     */
    std::tuple<bool, std::vector<uint8_t>, std::string>
        getIndirectStatusCommand();

    /**
     * @brief Checks if device is ready to accept the next transaction.
     * @return true if data is received, false otherwise.
     */
    bool isDeviceReadyForTx();

    /** @brief Print the buffer
     *
     *  @param isTx - True if the buffer is the command data written to the
     *device, false if the buffer is the data read from the device.
     *  @param buffer - Buffer to print
     */
    void printBuffer(bool isTx, const std::vector<uint8_t>& buffer);

  public:
    /**
     * @brief Constructor with parameters.
     * @param busAddr The bus address for I2C communication.
     * @param slaveAddr The slave address of the device.
     * @param verb Verbose logging flag.
     * @param emul Emulation mode flag.
     */
    OCPRecoveryCommands(int busAddr, int slaveAddr, bool verb, bool emul);
    OCPRecoveryCommands() = delete;
    OCPRecoveryCommands(const OCPRecoveryCommands&) = delete;
    OCPRecoveryCommands(OCPRecoveryCommands&&) = delete;
    OCPRecoveryCommands& operator=(const OCPRecoveryCommands&) = delete;
    OCPRecoveryCommands& operator=(OCPRecoveryCommands&&) = delete;

    /**
     * @brief Retrieves the device's status.
     * @return A tuple containing success flag, status data as a byte vector,
     * and an error message if any.
     */
    std::tuple<bool, std::vector<uint8_t>, std::string>
        getDeviceStatusCommand();

    /**
     * @brief Retrieves the recovery status of the device.
     * @return A tuple containing success flag, recovery status as a byte
     * vector, and an error message if any.
     */
    std::tuple<bool, std::vector<unsigned char>, std::string>
        getRecoveryStatusCommand();

    /**
     * @brief Initiates the recovery process using the specified image paths.
     * @param imagePaths A list of image paths to use for recovery.
     * @return A tuple containing success flag and an error message if any.
     */
    std::tuple<bool, std::string>
        performRecoveryCommand(const std::vector<std::string>& imagePaths);

    ~OCPRecoveryCommands();
};

} // namespace recovery_commands

} // namespace recovery_tool