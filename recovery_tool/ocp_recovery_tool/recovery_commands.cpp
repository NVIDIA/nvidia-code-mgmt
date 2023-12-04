#include "recovery_commands.hpp"

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

    return recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose);
}

bool OCPRecoveryCommands::writeRecoveryImage(
    const std::string& imageName, const std::vector<uint8_t>& imageData)
{
    constexpr size_t chunkSize = 252;
    size_t imageSize = imageData.size();
    for (size_t offset = 0; offset < imageSize; offset += chunkSize)
    {
        size_t remainingSize = imageSize - offset;
        size_t currentChunkSize = std::min(chunkSize, remainingSize);
        std::vector<uint8_t> dataChunk(imageData.begin() + offset,
                                       imageData.begin() + offset +
                                           currentChunkSize);

        // Logging payload in hex
        if (verbose)
        {
            for (const auto& byte : dataChunk)
            {
                std::cout << std::hex << static_cast<int>(byte) << " ";
            }
            std::cout << "\n";
        }

        int progress =
            static_cast<int>(((offset + currentChunkSize) * 100) / imageSize);

        std::cout << "\rWriting Image (" << imageName
                  << "), Progress: " << std::dec << progress << "% ("
                  << (offset + currentChunkSize) << "/" << imageSize
                  << " bytes) " << std::flush;
        if (emulation) // Added delay as GB100 emulation is slow so we are
                       // seeing issues with writing the data.
        {
            usleep(2000000);
        }
        if (!setIndirectDataCommand(dataChunk))
        {
            return false;
        }
    }
    std::cout << "\n";
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

        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
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

        if (recovery_tool::i2c_utils::sendI2cCmdForRead(
                i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
                readBuffer, verbose))
        {
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