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

#include "recovery_commands.hpp"

#include <fmt/format.h>

#include <chrono>
#include <thread>
namespace recovery_tool
{

namespace recovery_commands
{

std::string OCPRecoveryCommands::constructI2CDevicePath()
{
    return "/dev/i2c-" + std::to_string(busAddress);
}

utils::CustomFD OCPRecoveryCommands::openI2CDevice()
{
    std::string i2cDevicePath = constructI2CDevicePath();
    int fd = open(i2cDevicePath.c_str(), O_RDWR);
    if (fd < 0)
    {
        throw std::runtime_error("Failed to open device.");
    }
    return utils::CustomFD(fd);
}

void OCPRecoveryCommands::printBuffer(bool isTx,
                                      const std::vector<uint8_t>& buffer)
{
    if (!verbose)
    {
        return;
    }
    std::string formattedMessage = isTx ? "Tx: " : "Rx: ";
    // Reserve memory to minimize reallocations- buffer * 5 chars as each byte
    // in the buffer is formatted as "0xXX " + 4 for prefix ("Tx: " or "Rx: ")
    formattedMessage.reserve(buffer.size() * 5 + 4);
    for (const auto& byte : buffer)
    {
        formattedMessage += fmt::format("0x{:02x} ", byte);
    }
    if (!formattedMessage.empty())
    {
        formattedMessage.pop_back();
    }
    std::cout << formattedMessage << "\n";
}

bool OCPRecoveryCommands::setRecoveryControlRegisterCommand(ImageType imageType,
                                                            bool activation)
{
    std::vector<uint8_t> writeData(5);
    constexpr uint8_t dataSizeToWrite = 3; // size of data to be written
    writeData[0] = static_cast<uint8_t>(RecoveryCommands::RecoveryCtrl);
    writeData[1] = dataSizeToWrite;
    writeData[2] = static_cast<uint8_t>(imageType);
    writeData[3] = static_cast<uint8_t>(
        RecoveryImageSelection::FromMemoryWindow); // using image from CMS

    if (activation)
    {
        writeData[4] =
            static_cast<uint8_t>(ActivateRecoveryImage::ActivateImage);
    }
    else
    {
        writeData[4] =
            static_cast<uint8_t>(ActivateRecoveryImage::DoNotActivate);
    }
    printBuffer(Tx, writeData);

    if (emulation)
    {
        std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
    }

    auto fd = openI2CDevice();
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        fd(), static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

bool OCPRecoveryCommands::setIndirectControlRegisterCommand(ImageType imageType)
{

    std::vector<uint8_t> writeData = {
        static_cast<uint8_t>(RecoveryCommands::IndirectCtrl),
        0x6,                             // size of data to be written
        static_cast<uint8_t>(imageType), // cms
        0x0,                             // reserved -> 0
        0x0,                             // byte 2:5 -> 0 IMO
        0x0,                             // byte 2:5 -> 0 IMO
        0x0,                             // byte 2:5 -> 0 IMO
        0x0                              // byte 2:5 -> 0 IMO
    };
    printBuffer(Tx, writeData);
    auto fd = openI2CDevice();
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        fd(), static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

bool OCPRecoveryCommands::sendIndirectDataCommand(
    const std::vector<uint8_t>& data)
{
    constexpr size_t cmdHeaderSize = 2; // 2 for cmd_id and length of payload
    std::vector<uint8_t> writeData(cmdHeaderSize + data.size());

    writeData[0] = static_cast<uint8_t>(RecoveryCommands::IndirectData);
    writeData[1] = static_cast<uint8_t>(data.size());
    std::copy(data.begin(), data.end(), writeData.begin() + cmdHeaderSize);
    printBuffer(Tx, writeData);
    auto fd = openI2CDevice();
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        fd(), static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getIndirectStatusCommand()
{
    auto fd = openI2CDevice();
    try
    {
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::IndirectStatus)};
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::IndirectStatusResLen), 0);
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                fd(), static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
            printBuffer(Rx, readBuffer);
            return {true, readBuffer, ""};
        }
        auto errorMsg = "Failed to read data from device.";
        return {false, {}, errorMsg};
    }
    catch (const std::exception& e)
    {
        return {false, {}, std::string(e.what())};
    }
}

bool OCPRecoveryCommands::isDeviceReadyForTx()
{

    if (emulation) // Added delay as some machines emulation are slow so we are
                   // seeing issues with reading the status just after writing
                   // the data
    {
        std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
    }
    static constexpr size_t maxRetriesForWritingData = 5;
    for (size_t i = 0; i < maxRetriesForWritingData; ++i)
    {
        if ((i > 0) && verbose)
        {
            std::cout
                << "Retry #" << i
                << ": Verifying ack from device in a polling address space.\n";
        }
        auto [success, hexResponse, errorMsg] = getIndirectStatusCommand();
        if (!success)
        {
            if (verbose)
            {
                std::cerr << "Error in getIndirectStatusCommand: " << errorMsg
                          << "\n";
            }
            return false;
        }
        constexpr uint8_t mask = 0x4;
        constexpr uint8_t shift = 2;
        // Extract the ACK from device bit (bit 2) from the second byte of
        // hexResponse.
        auto indirectStatusAck = (hexResponse[1] & mask) >> shift;
        if (indirectStatusAck == indirectStatusExpectedAck)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
    }
    if (verbose)
    {
        std::cerr
            << "TimeoutError: ACK not received from device in a polling address space.\n";
    }
    return false;
}

bool OCPRecoveryCommands::writeRecoveryChunk(
    const std::string_view imageName, const std::string_view targetName,
    const std::vector<uint8_t>& imageData, const size_t offset)
{
    size_t imageSize = imageData.size();
    size_t remainingSize = imageSize - offset;
    size_t currentChunkSize = std::min(chunkSize, remainingSize);
    std::vector<uint8_t> dataChunk(imageData.begin() + offset,
                                   imageData.begin() + offset +
                                       currentChunkSize);

    if (offset == 0)
    {
        lastLoggedProgress = 0;
    }

    uint8_t progress =
        static_cast<uint8_t>(((offset + currentChunkSize) * 100) / imageSize);

    if ((progress / 10) > (lastLoggedProgress / 10))
    {
        lastLoggedProgress = progress;
        std::string progressMessage = fmt::format(
            "Writing Image ({} to {}), Progress: {}% ({} / {} bytes)",
            imageName, targetName, progress, (offset + currentChunkSize),
            imageSize);
        std::cout << progressMessage << "\n";
    }

    static constexpr size_t retryAttemptsPerChunk = 3;
    bool writeStatus = false;
    for (size_t attempt = 0; attempt < retryAttemptsPerChunk; ++attempt)
    {
        if (!sendIndirectDataCommand(dataChunk))
        {
            continue;
        }
        if (!isDeviceReadyForTx())
        {
            continue;
        }
        writeStatus = true;
        break;
    }
    return writeStatus;
}

bool OCPRecoveryCommands::writeRecoveryImage(
    const std::string& imageName, const std::string& targetName,
    const std::vector<uint8_t>& imageData)
{
    size_t imageSize = imageData.size();
    std::cout << "Initiating recovery image write process...\n";

    for (size_t offset = 0; offset < imageSize; offset += chunkSize)
    {
        if (!writeRecoveryChunk(imageName, targetName, imageData, offset))
        {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t>
    OCPRecoveryCommands::readFirmwareImage(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }

    std::vector<uint8_t> data = {std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>()};

    if (!file.good() && !file.eof())
    {
        return {};
    }
    return data;
}

OCPRecoveryCommands::OCPRecoveryCommands(int busAddr, int slaveAddr, bool verb,
                                         bool emul) :
    busAddress(busAddr), slaveAddress(slaveAddr), verbose(verb), emulation(emul)
{}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getDeviceIDCommand()
{
    std::string errorMsg = "";
    auto fd = openI2CDevice();
    try
    {
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::DeviceID)};
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::DeviceIDResLen), 0);
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                fd(), static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
            printBuffer(Rx, readBuffer);
            return {true, readBuffer, errorMsg};
        }
        errorMsg = "Failed to read data from device.";
        return {false, {}, errorMsg};
    }
    catch (const std::exception& e)
    {
        errorMsg = "Error in GetDeviceId: " + std::string(e.what());
        return {false, {}, errorMsg};
    }
}

std::pair<bool, std::string> OCPRecoveryCommands::setForceRecoveryMode()
{
    std::string errorMsg = "";
    std::vector<uint8_t> writeData(commandCodeLen + commandBytesWrittenLen +
                                   resetCommandDataLen);
    writeData[0] = static_cast<uint8_t>(RecoveryCommands::Reset);
    writeData[1] = resetCommandDataLen;
    writeData[2] = static_cast<uint8_t>(0x01); // Reset Device
    writeData[3] = static_cast<uint8_t>(0x0F); // Enter Recovery Mode on Reset
    writeData[4] = static_cast<uint8_t>(0x01); // Enable Interface Mastering
    printBuffer(Tx, writeData);
    auto fd = openI2CDevice();
    try
    {
        if (!recovery_tool::i2c_utils::sendI2cCmdForWrite(
                fd(), static_cast<uint16_t>(slaveAddress), writeData, verbose))
        {
            return {false, "Failed to set device into recovery mode"};
        }
    }
    catch (const std::exception& e)
    {
        errorMsg = "Failed to set device into recovery mode : " +
                   std::string(e.what());
        return {false, errorMsg};
    }
    return {true, ""};
}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getDeviceStatusCommand()
{
    std::string errorMsg = "";
    auto fd = openI2CDevice();
    try
    {
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::DeviceStatus)};
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::DeviceStatusResLen), 0);
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                fd(), static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
            printBuffer(Rx, readBuffer);
            return {true, readBuffer, errorMsg};
        }
        errorMsg = "Failed to read data from device.";
        return {false, {}, errorMsg};
    }
    catch (const std::exception& e)
    {
        errorMsg = "Error in Get Device Status: " + std::string(e.what());
        return {false, {}, errorMsg};
    }
}

std::tuple<bool, std::vector<unsigned char>, std::string>
    OCPRecoveryCommands::getRecoveryStatusCommand()
{
    std::string errorMsg = "";
    auto fd = openI2CDevice();
    try
    {
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::RecoveryStatusResLen),
            0); // Buffer to store read data
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::RecoveryStatus)};
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                fd(), static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
            printBuffer(Rx, readBuffer);
            return {true, readBuffer, errorMsg};
        }
        errorMsg = "Failed to read data from device.";
        return {false, {}, errorMsg};
    }
    catch (const std::exception& e)
    {
        errorMsg = "Error in Get Recovery Status: " + std::string(e.what());
        return {false, {}, errorMsg};
    }
}

std::tuple<bool, std::string> OCPRecoveryCommands::performRecoveryCommand(
    const std::vector<std::string>& imagePaths)
{
    std::string errorMsg = "";
    try
    {
        for (size_t index = 0; index < imagePaths.size(); ++index)
        {
            const auto& imagePath = imagePaths[index];

            // Check if image file exists
            if (!std::filesystem::exists(imagePath))
            {
                errorMsg = "Image does not exist: " + imagePath;
                return {false, errorMsg};
            }

            // Read image data
            std::vector<uint8_t> imageBytes = readFirmwareImage(imagePath);
            if (imageBytes.empty())
            {
                errorMsg =
                    "Failed to read data from the image file or file is empty: " +
                    imagePath;
                return {false, errorMsg};
            }
            // Configure recovery control register
            if (!setRecoveryControlRegisterCommand(ImageType::CMS0, false))
            {
                errorMsg = "Writing to RecoveryControlRegister failed for " +
                           imagePath;
                return {false, errorMsg};
            }

            // Configure indirect control register
            if (!setIndirectControlRegisterCommand(ImageType::CMS0))
            {
                errorMsg = "Writing to IndirectControlRegister failed for " +
                           imagePath;
                return {false, errorMsg};
            }
            auto targetName = "CMS0";
            if (!writeRecoveryImage(imagePath, targetName, imageBytes))
            {
                errorMsg = "Writing " + imagePath + " recovery image failed";
                return {false, errorMsg};
            }
            if (!setRecoveryControlRegisterCommand(ImageType::CMS0, true))
            {
                errorMsg =
                    "Activating " + imagePath + " recovery image failed.";
                return {false, errorMsg};
            }
        }

        return {true, ""};
    }
    catch (const std::exception& e)
    {
        errorMsg = "Error in Performing Recovery: " + std::string(e.what());
        return {false, errorMsg};
    }
}

std::pair<bool, std::string>
    OCPRecoveryCommands::saveToLogFile(const std::vector<uint8_t>& hexData,
                                       const std::string& filePath)
{
    std::string errorMsg = "";
    std::ofstream outFile(filePath);
    if (outFile.is_open())
    {
        std::stringstream ss;
        for (const auto& byte : hexData)
        {
            ss << "0x" << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte) << " ";
        }
        outFile << ss.str();
        outFile.close();
    }
    else
    {
        errorMsg = "Failed to open " + filePath + " for writing.";
        return {false, errorMsg};
    }
    return {true, errorMsg};
}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getCMSLogs(const uint8_t window)
{

    std::string errorMsg = "";

    if (!setIndirectControlRegisterCommand(static_cast<ImageType>(window)))
    {
        errorMsg = "Failed to set cms2 in the INDIRECT_CTRL register";
        return {false, {}, errorMsg};
    }

    std::vector<uint8_t> combinedLogs{};
    std::vector<uint8_t> readBuffer(
        static_cast<size_t>(
            recovery_tool::recovery_commands::ResponseLength::CMSLogsChunkSize),
        0);
    std::vector<uint8_t> commandData = {
        static_cast<uint8_t>(RecoveryCommands::IndirectData)};

    auto fd = openI2CDevice();
    for (int i = 0; i < numOfReadsForCMSLogs; ++i)
    {
        if (!recovery_tool::i2c_utils::sendI2cCmdForRead(
                fd(), static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
            std::string errorMsg = "Failed to read " + std::to_string(i + 1) +
                                   "/3 chunk of CMS2 logs";
            return {false, {}, errorMsg};
        }
        combinedLogs.insert(combinedLogs.end(), readBuffer.begin(),
                            readBuffer.end());
        // Clear contents by filling with zeros
        std::fill(readBuffer.begin(), readBuffer.end(), 0);
    }

    return {true, combinedLogs, ""};
}

} // namespace recovery_commands
} // namespace recovery_tool