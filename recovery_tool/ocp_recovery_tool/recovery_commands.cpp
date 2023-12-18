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

void OCPRecoveryCommands::openI2CDevice()
{
    std::string i2cDevicePath = constructI2CDevicePath();
    i2cFile = open(i2cDevicePath.c_str(), O_RDWR);
    if (i2cFile < 0)
    {
        throw std::runtime_error("Failed to open device.");
    }
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
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose);
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
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

bool OCPRecoveryCommands::setIndirectDataCommand(
    const std::vector<uint8_t>& data)
{
    constexpr size_t cmdHeaderSize = 2; // 2 for cmd_id and length of payload
    std::vector<uint8_t> writeData(cmdHeaderSize + data.size());

    writeData[0] = static_cast<uint8_t>(RecoveryCommands::IndirectData);
    writeData[1] = static_cast<uint8_t>(data.size());
    std::copy(data.begin(), data.end(), writeData.begin() + cmdHeaderSize);
    printBuffer(Tx, writeData);
    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getIndirectStatusCommand()
{
    try
    {
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::IndirectStatus)};
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::IndirectStatusResLen), 0);
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
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

    if (emulation) // Added delay as GB100 emulation is slow so we are
                   // seeing issues with reading the status just after writing the data
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
        // Extract the ACK from device bit (bit 2) from the second byte of hexResponse.
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

bool OCPRecoveryCommands::writeRecoveryImage(
    const std::string& imageName, const std::vector<uint8_t>& imageData)
{
    constexpr size_t chunkSize = 252;
    size_t imageSize = imageData.size();
    uint8_t lastLoggedProgress = 0;
    std::cout << "Initiating recovery image write process...\n";
    for (size_t offset = 0; offset < imageSize; offset += chunkSize)
    {
        size_t remainingSize = imageSize - offset;
        size_t currentChunkSize = std::min(chunkSize, remainingSize);
        std::vector<uint8_t> dataChunk(imageData.begin() + offset,
                                       imageData.begin() + offset +
                                           currentChunkSize);

        uint8_t progress =
            static_cast<uint8_t>(((offset + currentChunkSize) * 100) / imageSize);

        if ((progress / 10) > (lastLoggedProgress / 10))
        {
            lastLoggedProgress = progress;
            std::string progressMessage = fmt::format(
                "Writing Image ({}), Progress: {}% ({} / {} bytes)", imageName,
                progress, (offset + currentChunkSize), imageSize);
            std::cout << progressMessage << "\n";
        }
        if (!setIndirectDataCommand(dataChunk))
        {
            return false;
        }
        if (!isDeviceReadyForTx())
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
    busAddress(busAddr),
    slaveAddress(slaveAddr), verbose(verb), emulation(emul)
{
    openI2CDevice();
}

OCPRecoveryCommands::~OCPRecoveryCommands()
{
    close(i2cFile);
}

std::tuple<bool, std::vector<uint8_t>, std::string>
    OCPRecoveryCommands::getDeviceStatusCommand()
{
    std::string errorMsg = "";
    try
    {
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::DeviceStatus)};
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::DeviceStatusResLen), 0);
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
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
    try
    {
        std::vector<uint8_t> readBuffer(
            static_cast<size_t>(ResponseLength::RecoveryStatusResLen),
            0); // Buffer to store read data
        std::vector<uint8_t> commandData = {
            static_cast<uint8_t>(RecoveryCommands::RecoveryStatus)};
        printBuffer(Tx, commandData);
        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
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
            const auto& path = imagePaths[index];
            if (!std::filesystem::exists(path))
            {
                errorMsg = "Image does not exist: " + path;
                return {false, errorMsg};
            }

            std::vector<uint8_t> imageBytes = readFirmwareImage(path);

            if (imageBytes.empty())
            {
                errorMsg =
                    "Failed to read data from the image file or file is empty: " +
                    path;
                return {false, errorMsg};
            }
            ImageType imageType = static_cast<ImageType>(index);
            if (!setRecoveryControlRegisterCommand(imageType, false))
            {
                errorMsg =
                    "Writing to RecoveryControlRegister failed for " + path;
                return {false, errorMsg};
            }
            if (!setIndirectControlRegisterCommand(imageType))
            {
                errorMsg =
                    "Writing to IndirectControlRegister failed for " + path;
                return {false, errorMsg};
            }
            auto imageName = "CMS" + std::to_string(index);
            if (!writeRecoveryImage(imageName, imageBytes))
            {
                errorMsg = "Writing recovery image failed for " + path;
                return {false, errorMsg};
            }
        }
        if (!setRecoveryControlRegisterCommand(ImageType::CMS0, true))
        {
            errorMsg = "Activating recovery image failed.";
            return {false, errorMsg};
        }
        return {true, ""};
    }
    catch (const std::exception& e)
    {
        errorMsg = "Error in Performing Recovery: " + std::string(e.what());
        return {false, errorMsg};
    }
}

} // namespace recovery_commands
} // namespace recovery_tool