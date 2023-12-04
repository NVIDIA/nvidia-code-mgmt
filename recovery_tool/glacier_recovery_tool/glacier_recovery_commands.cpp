#include "glacier_recovery_commands.hpp"

#include <fmt/format.h>

#include <array>
#include <unordered_set>

namespace glacier_recovery_tool
{

namespace glacier_recovery_commands
{

/** CRC32 code derived from work by Gary S. Brown.
 *  http://web.mit.edu/freebsd/head/sys/libkern/crc32.c
 *
 *  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or
 *  code or tables extracted from it, as desired without restriction.
 *
 */
static uint32_t crc32_tab[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d};

uint32_t crc32(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~0U;
    while (size--)
        crc = crc32_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ ~0U;
}

void GlacierRecoveryCommands::openI2CDevice()
{
    auto i2cDevicePath = "/dev/i2c-" + std::to_string(busAddress);
    i2cFile = open(i2cDevicePath.c_str(), O_RDWR);
    if (i2cFile < 0)
    {
        throw std::runtime_error("Failed to open device.");
    }
}

std::string GlacierRecoveryCommands::recoveryResultToStr(RecoveryResult result)
{
    switch (result)
    {
        case RecoveryResult::Ok:
            return "Operation successful.";
        case RecoveryResult::IllegalPayloadLength:
            return "Payload length exceeds the allocated space.";
        case RecoveryResult::CRCFailure:
            return "CRC check failed on the command parameters.";
        case RecoveryResult::ResponseCRCCompFailure:
            return "CRC mismatch between the request and response.";
        case RecoveryResult::IllegalHeaderOffset:
            return "Header offset exceeds the maximum allowed (0x37F).";
        case RecoveryResult::IllegalKeyHashBlobOffset:
            return "Key Hash Blob offset exceeds the maximum allowed limit.";
        case RecoveryResult::IllegalFWImageWriteAddress:
            return "Firmware image address exceeds the maximum allocated address.";
        case RecoveryResult::InvalidCommandSignature:
            return "Command signature failed the authentication check using the Platform Command Key.";
        case RecoveryResult::DeviceNotInRecovery:
            return "Device is not in Recovery.";
        case RecoveryResult::InitResponseByteMismatch:
            return "Initial response byte does not match expected value.";
        case RecoveryResult::BadResponse:
            return "Incorrect response received; bytes 3-6 do not match the expected pattern.";
        case RecoveryResult::InvalidCommand:
            return "The command issued is invalid.";
        case RecoveryResult::Pending:
            return "Command timeout; no response received for the last command.";
        case RecoveryResult::FailedToReadData:
            return "Failed to read data from the device.";
        case RecoveryResult::InvalidRevision:
            return "The firmware revision is invalid.";
        case RecoveryResult::SRAMCmdFailed:
            return "SRAM command execution failed.";
        case RecoveryResult::FileOpenFailure:
            return "Unable to open the recovery image file.";
        case RecoveryResult::FailedToReadVendorDetails:
            return "Failed to read the vendor details.";
        case RecoveryResult::FailedToReadHeader:
            return "Failed to read the image header.";
        case RecoveryResult::FailedToReadKHB:
            return "Failed to read the Key Hash Blob.";
        case RecoveryResult::FailedToReadFWImage:
            return "Failed to read the firmware image.";
        default:
            return "An unknown issue occurred.";
    }
}

void GlacierRecoveryCommands::logVerbose(const std::string& message) const
{
    if (verbose)
    {
        std::cout << message << "\n";
    }
}

void GlacierRecoveryCommands::printBuffer(bool isTx,
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
    logVerbose(formattedMessage);
}

std::vector<uint8_t> GlacierRecoveryCommands::getCommandBytesWithCRC32(
    RecoveryCommand recoveryCommand, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> commandBytes{static_cast<uint8_t>(recoveryCommand)};
    commandBytes.insert(commandBytes.end(), payload.begin(), payload.end());

    uint32_t crc32Val = crc32(commandBytes.data(), commandBytes.size());

    for (int i = 0; i < 4; ++i)
    {
        commandBytes.push_back(static_cast<uint8_t>(crc32Val & 0xFF));
        crc32Val >>= 8;
    }

    return commandBytes;
}

std::vector<uint8_t>
    GlacierRecoveryCommands::GetResponse(ResponseLength responseLen)
{
    auto commandData =
        getCommandBytesWithCRC32(RecoveryCommand::GetResponse, {});
    std::vector<uint8_t> readBuffer(static_cast<size_t>(responseLen), 0);
    printBuffer(Tx, commandData);
    if (recovery_tool::i2c_utils::sendI2cCmdForRead(
            i2cFile, static_cast<uint16_t>(slaveAddress), commandData,
            readBuffer, verbose))
    {
        printBuffer(Rx, readBuffer);
        return readBuffer;
    }
    else
    {
        throw std::runtime_error(
            recoveryResultToStr(RecoveryResult::FailedToReadData));
    }
}

RecoveryResult GlacierRecoveryCommands::ValidateGetResponseCmd(
    RecoveryCommand command, const std::vector<uint8_t>& responseBytes,
    ResponseLength responseLen)
{
    if (responseBytes[1] == static_cast<uint8_t>(RecoveryCommand::GetResponse))
    {
        return RecoveryResult::Pending;
    }
    if (responseBytes[1] != static_cast<uint8_t>(command))
    {
        return RecoveryResult::DeviceNotInRecovery;
    }

    auto length = static_cast<size_t>(responseLen);
    if (responseBytes.size() != length)
    {
        return RecoveryResult::IllegalPayloadLength;
    }
    auto commandBytes =
        std::vector<uint8_t>(responseBytes.begin(), responseBytes.end() - 4);
    uint32_t respCrc = crc32(commandBytes.data(), commandBytes.size());
    uint32_t packCrc =
        ((responseBytes[length - 1] << 24) | (responseBytes[length - 2] << 16) |
         (responseBytes[length - 3] << 8) | responseBytes[length - 4]);

    if (respCrc != packCrc)
    {
        std::cout << "ValidateCRC failed"
                  << fmt::format("{:08x} != {:08x}\n", respCrc, packCrc);
        return RecoveryResult::ResponseCRCCompFailure;
    }

    if (command == RecoveryCommand::Initialization)
    {
        if ((responseBytes[2] != initResponseByte1) ||
            (responseBytes[3] != initResponseByte2))
        {
            return RecoveryResult::InitResponseByteMismatch;
        }
    }

    if (command == RecoveryCommand::GetFWInfo)
    {
        if ((responseBytes[3] != 0x50) || (responseBytes[4] != 0x48) ||
            (responseBytes[5] != 0x43) || (responseBytes[6] != 0x4D))
        {
            return RecoveryResult::BadResponse;
        }
    }

    static const std::unordered_set<RecoveryCommand> writeImageCommands = {
        RecoveryCommand::HeaderWrite, RecoveryCommand::KeyHashBlobWrite,
        RecoveryCommand::FWImageWrite};
    if (writeImageCommands.contains(command))
    {
        return ValidateWriteImageResponse(command, responseBytes);
    }
    return RecoveryResult::Ok;
}

RecoveryResult GlacierRecoveryCommands::ValidateWriteImageResponse(
    RecoveryCommand cmd, const std::vector<uint8_t>& responseBytes)
{
    if (responseBytes[2] & crcFailField)
    {
        return RecoveryResult::CRCFailure;
    }
    if (responseBytes[2] & illegalLengthField)
    {
        return RecoveryResult::IllegalPayloadLength;
    }
    if (responseBytes[2] & invalidCmdSignatureField)
    {
        return RecoveryResult::InvalidCommandSignature;
    }
    if (responseBytes[2] & illegalOffsetField)
    {
        switch (cmd)
        {
            case RecoveryCommand::HeaderWrite:
                return RecoveryResult::IllegalHeaderOffset;
            case RecoveryCommand::KeyHashBlobWrite:
                return RecoveryResult::IllegalKeyHashBlobOffset;
            case RecoveryCommand::FWImageWrite:
                return RecoveryResult::IllegalFWImageWriteAddress;
            default:
                return RecoveryResult::InvalidCommand;
        }
    }
    return RecoveryResult::Ok;
}

RecoveryResult
    GlacierRecoveryCommands::executeGetResponseCmdAndValidateResponse(
        RecoveryCommand cmd, ResponseLength responseLen)
{
    uint8_t retries = 0;
    while (retries < maxRetries)
    {
        auto resBytes = GetResponse(responseLen);
        auto result = ValidateGetResponseCmd(cmd, resBytes, responseLen);
        if (result != RecoveryResult::Pending)
        {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
        retries++;
    }
    return RecoveryResult::Pending;
}

bool GlacierRecoveryCommands::performSRAMExe()
{
    std::vector<uint8_t> writeData{
        static_cast<uint8_t>(RecoveryCommand::SRAMExe)};
    printBuffer(Tx, writeData);
    return (recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose));
}

GlacierRecoveryCommands::GlacierRecoveryCommands(int busAdd, int slaveAdd,
                                                 bool verbose) :
    busAddress(busAdd),
    slaveAddress(slaveAdd), verbose(verbose)
{
    openI2CDevice();
};

GlacierRecoveryCommands::~GlacierRecoveryCommands()
{
    close(i2cFile);
}

RecoveryResult GlacierRecoveryCommands::performInitialization()
{
    auto writeData = getCommandBytesWithCRC32(RecoveryCommand::Initialization,
                                              {initResponseByte0});
    printBuffer(Tx, writeData);
    if (!(recovery_tool::i2c_utils::sendI2cCmdForWrite(
            i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose)))
    {
        return RecoveryResult::FailedToReadData;
    }

    auto recoveryResult = executeGetResponseCmdAndValidateResponse(
        RecoveryCommand::Initialization, ResponseLength::InitResponse);

    return recoveryResult;
}

std::tuple<RecoveryResult, std::vector<uint8_t>>
    GlacierRecoveryCommands::getFirmwareInfoCommand()
{

    auto writeData = getCommandBytesWithCRC32(RecoveryCommand::GetFWInfo, {});
    printBuffer(Tx, writeData);
    if (!(recovery_tool::i2c_utils::sendI2cCmdForWrite(
            i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose)))
    {
        return {RecoveryResult::FailedToReadData, {}};
    }
    ResponseLength resLen;
    if (revision == Revision::RevA)
    {
        resLen = ResponseLength::FwInfoRevAResponse;
    }
    else if (revision == Revision::RevB)
    {
        resLen = ResponseLength::FwInfoRevBResponse;
    }
    else
    {
        return {RecoveryResult::InvalidRevision, {}};
    }

    std::vector<uint8_t> hexResponse{};
    uint8_t retries = 0;
    RecoveryResult recoveryResult = RecoveryResult::Ok;
    while (retries < maxRetries)
    {
        hexResponse = GetResponse(resLen);
        auto recoveryResult = ValidateGetResponseCmd(RecoveryCommand::GetFWInfo,
                                                     hexResponse, resLen);
        if (recoveryResult != RecoveryResult::Pending)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
        retries++;
    }

    if (recoveryResult != RecoveryResult::Ok)
    {
        return {recoveryResult, {}};
    }

    return {recoveryResult, hexResponse};
}

std::vector<uint8_t> GlacierRecoveryCommands::toOffsetBytes(size_t value)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(3);
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    return bytes;
}

std::vector<uint8_t> GlacierRecoveryCommands::getWriteCommandPayload(
    const std::vector<uint8_t>& inputPayload, size_t offset)
{

    auto paloadLen = inputPayload.size() - 1;
    std::vector<uint8_t> payload;

    payload.reserve(offsetBytes + payloadLengthBytes + inputPayload.size() +
                    randomByteCount);

    auto offsetData = toOffsetBytes(offset);

    payload.insert(payload.end(), offsetData.begin(), offsetData.end());
    payload.push_back(static_cast<uint8_t>(paloadLen));
    payload.insert(payload.end(), inputPayload.begin(), inputPayload.end());

    std::vector<uint8_t> randomBytes(randomByteCount, 0);
    payload.insert(payload.end(), randomBytes.begin(), randomBytes.end());

    return payload;
}

RecoveryResult GlacierRecoveryCommands::executeWriteImageCmd(
    RecoveryCommand cmd, const std::vector<uint8_t>& dataChunk, size_t offset)
{

    auto payload = getWriteCommandPayload(dataChunk, offset);
    auto writeData = getCommandBytesWithCRC32(cmd, payload);
    printBuffer(Tx, writeData);
    if (!(recovery_tool::i2c_utils::sendI2cCmdForWrite(
            i2cFile, static_cast<uint16_t>(slaveAddress), writeData, verbose)))
    {
        return RecoveryResult::FailedToReadData;
    }
    auto recoveryResult = executeGetResponseCmdAndValidateResponse(
        cmd, ResponseLength::WriteResponse);
    return recoveryResult;
}

RecoveryResult
    GlacierRecoveryCommands::writeImage(RecoveryCommand cmd, size_t size,
                                        const std::vector<uint8_t>& data)
{
    size_t base = 0;
    size_t chunk = defaultChunkSize;
    for (size_t i = 0; i < size; i += chunk)
    {
        // Calculate the chunk size (might be less than 'chunk' for the last
        // piece)
        size_t currentChunkSize = std::min(chunk, size - i);
        std::vector<uint8_t> currentChunk(data.begin() + i,
                                          data.begin() + i + currentChunkSize);

        auto recoveryResult = executeWriteImageCmd(cmd, currentChunk, base + i);
        if (recoveryResult != RecoveryResult::Ok)
        {
            return recoveryResult;
        }
    }
    return RecoveryResult::Ok;
}

RecoveryResult GlacierRecoveryCommands::performGlacierRecovery(
    const std::string& imgFilePath)
{
    std::ifstream fd(imgFilePath, std::ios::binary);
    if (!fd)
    {
        return RecoveryResult::FileOpenFailure;
    }

    uint32_t imgOffset;
    fd.read(reinterpret_cast<char*>(&imgOffset), 3);
    imgOffset = imgOffset * 256;
    logVerbose("Image offset: " + fmt::format("0x{:02x} ", imgOffset));
    fd.seekg(imgOffset);

    std::vector<char> vendor(4);
    if (!fd.read(vendor.data(), 4))
    {
        return RecoveryResult::FailedToReadVendorDetails;
    }

    std::string vendorStr(vendor.begin(), vendor.end());
    logVerbose("0x00: Vendor: " + vendorStr);

    uint32_t version;
    fd.read(reinterpret_cast<char*>(&version), 4);
    logVerbose("0x04: Version: " + fmt::format("0x{:02x} ", (version & 0xff)));

    uint32_t loadAddr;
    fd.read(reinterpret_cast<char*>(&loadAddr), 4);
    logVerbose("0x08: Image Load Address: " +
               fmt::format("0x{:02x} ", loadAddr));

    uint32_t entryAddr;
    fd.read(reinterpret_cast<char*>(&entryAddr), 4);
    logVerbose("0x0C: Image Entry Address: " +
               fmt::format("0x{:02x} ", entryAddr));

    uint32_t fwBinLen;
    fd.read(reinterpret_cast<char*>(&fwBinLen), 4);
    fwBinLen = (fwBinLen & 0xFFFF) * 128;
    logVerbose("0x10: Firmware binary Length: " +
               fmt::format("0x{:02x} ", fwBinLen));

    fd.ignore(0x18);

    uint32_t blobAddr;
    fd.read(reinterpret_cast<char*>(&blobAddr), 4);
    logVerbose("0x2C: Hash Blob 0 Address: " +
               fmt::format("0x{:02x} ", blobAddr));

    fd.seekg(imgOffset);
    std::vector<uint8_t> headerBin(headerLength);
    if (!fd.read(reinterpret_cast<char*>(headerBin.data()), headerLength))
    {
        return RecoveryResult::FailedToReadHeader;
    }

    std::vector<uint8_t> fwBin(fwBinLen + defaultChunkSize);
    if (!fd.read(reinterpret_cast<char*>(fwBin.data()),
                 fwBinLen + defaultChunkSize))
    {
        return RecoveryResult::FailedToReadFWImage;
    }

    fd.seekg(blobAddr);
    uint32_t blobSize =
        (revision == Revision::RevA) ? revAKHBSize : revBKHBSize;
    std::vector<uint8_t> blobBin(blobSize);
    if (!fd.read(reinterpret_cast<char*>(blobBin.data()), blobSize))
    {
        return RecoveryResult::FailedToReadKHB;
    }

    logVerbose("Writing Key Hash Blob");

    auto writeKHBStatus =
        writeImage(RecoveryCommand::KeyHashBlobWrite, blobBin.size(), blobBin);
    if (writeKHBStatus != RecoveryResult::Ok)
    {
        return writeKHBStatus;
    }
    logVerbose("Writing Header");

    auto writeHeaderStatus =
        writeImage(RecoveryCommand::HeaderWrite, headerBin.size(), headerBin);
    if (writeHeaderStatus != RecoveryResult::Ok)
    {
        return writeHeaderStatus;
    }
    logVerbose("Writing Firmware Image");

    auto writeFWImageStatus =
        writeImage(RecoveryCommand::FWImageWrite, fwBin.size(), fwBin);
    if (writeFWImageStatus != RecoveryResult::Ok)
    {
        return writeFWImageStatus;
    }
    if (!performSRAMExe())
    {
        return RecoveryResult::SRAMCmdFailed;
    }
    logVerbose("Executed SRA_EXE command successfully");

    return RecoveryResult::Ok;
}

bool GlacierRecoveryCommands::unlockI2CDevice()
{
    std::vector<uint8_t> writeData{0xc0, 0x01};
    printBuffer(Tx, writeData);
    return (recovery_tool::i2c_utils::sendI2cCmdForWrite(
        i2cFile, static_cast<uint16_t>(RecoveryCommand::ShowHiddenERoTs),
        writeData, verbose));
}

} // namespace glacier_recovery_commands
} // namespace glacier_recovery_tool
