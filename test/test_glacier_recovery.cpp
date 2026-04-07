#include <fcntl.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#define private public
#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_commands.hpp"
#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_utils.hpp"
#undef private

namespace
{

struct ReadBehavior
{
    bool success = true;
    bool throws = false;
    std::vector<uint8_t> data;
};

std::deque<bool> writeResults;
std::deque<bool> writeThrows;
std::deque<ReadBehavior> readResults;
std::vector<std::vector<uint8_t>> capturedWrites;
std::vector<std::vector<uint8_t>> capturedReadCommands;
std::vector<std::string> openedPaths;
std::vector<int> closedFds;
int openResult = 61;

void writeLe32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value)
{
    buffer[offset] = static_cast<uint8_t>(value & 0xFF);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::filesystem::path writeGlacierImage(std::string_view name)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    std::vector<uint8_t> image(3000, 0);
    for (size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<uint8_t>((i * 3) & 0xFF);
    }

    image[0] = 1; // imgOffset = 1 * 256
    image[1] = 0;
    image[2] = 0;
    constexpr size_t imgOffset = 256;
    constexpr uint32_t blobAddr = 2048;

    image[imgOffset + 0] = 'P';
    image[imgOffset + 1] = 'H';
    image[imgOffset + 2] = 'C';
    image[imgOffset + 3] = 'M';

    writeLe32(image, imgOffset + 4, 0x00000001);  // version
    writeLe32(image, imgOffset + 8, 0x00000010);  // loadAddr
    writeLe32(image, imgOffset + 12, 0x00000020); // entryAddr
    writeLe32(image, imgOffset + 16, 0x00000001); // fwBinLen words => 128
    writeLe32(image, imgOffset + 0x2C, blobAddr);

    const auto path =
        std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    return path;
}

std::vector<uint8_t> makeResponse(
    glacier_recovery_tool::glacier_recovery_commands::RecoveryCommand command,
    size_t length, const std::vector<uint8_t>& payload = {})
{
    std::vector<uint8_t> response(length, 0);
    response[1] = static_cast<uint8_t>(command);
    for (size_t i = 0; i < payload.size() && (2 + i) < (length - 4); ++i)
    {
        response[2 + i] = payload[i];
    }

    const uint32_t crc =
        glacier_recovery_tool::glacier_recovery_commands::crc32(
            response.data(), response.size() - 4);
    response[length - 4] = static_cast<uint8_t>(crc & 0xFF);
    response[length - 3] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    response[length - 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    response[length - 1] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    return response;
}

std::vector<uint8_t> makeWriteStatusResponse(
    glacier_recovery_tool::glacier_recovery_commands::RecoveryCommand command,
    uint8_t status)
{
    auto response = makeResponse(command, 7);
    response[2] = status;
    const uint32_t crc =
        glacier_recovery_tool::glacier_recovery_commands::crc32(
            response.data(), response.size() - 4);
    response[3] = static_cast<uint8_t>(crc & 0xFF);
    response[4] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    response[5] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    response[6] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    return response;
}

void queueGlacierWriteResponses()
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    readResults.clear();
    for (size_t i = 0; i < 5; ++i)
    {
        readResults.push_back(ReadBehavior{
            .success = true,
            .data = makeResponse(RecoveryCommand::KeyHashBlobWrite, 7)});
    }
    for (size_t i = 0; i < 7; ++i)
    {
        readResults.push_back(ReadBehavior{
            .success = true,
            .data = makeResponse(RecoveryCommand::HeaderWrite, 7)});
    }
    for (size_t i = 0; i < 2; ++i)
    {
        readResults.push_back(ReadBehavior{
            .success = true,
            .data = makeResponse(RecoveryCommand::FWImageWrite, 7)});
    }
}

class GlacierRecoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        writeResults.clear();
        writeThrows.clear();
        readResults.clear();
        capturedWrites.clear();
        capturedReadCommands.clear();
        openedPaths.clear();
        closedFds.clear();
        openResult = 61;
    }
};

class ThrowingStreamBuf : public std::streambuf
{
  protected:
    std::streamsize xsputn(const char*, std::streamsize) override
    {
        throw std::runtime_error("fake stdout failure");
    }

    int overflow(int) override
    {
        throw std::runtime_error("fake stdout failure");
    }
};

class StreamFailureGuard
{
  public:
    explicit StreamFailureGuard(std::ostream& stream) :
        stream(stream), oldBuffer(stream.rdbuf())
    {
        stream.rdbuf(&throwingBuffer);
    }

    ~StreamFailureGuard()
    {
        stream.rdbuf(oldBuffer);
    }

  private:
    std::ostream& stream;
    std::streambuf* oldBuffer;
    ThrowingStreamBuf throwingBuffer;
};

class PropagatingStreamFailureGuard
{
  public:
    explicit PropagatingStreamFailureGuard(std::ostream& stream) :
        stream(stream), oldBuffer(stream.rdbuf()),
        oldExceptions(stream.exceptions())
    {
        stream.rdbuf(&throwingBuffer);
        stream.exceptions(oldExceptions | std::ios::badbit | std::ios::failbit);
    }

    ~PropagatingStreamFailureGuard()
    {
        stream.exceptions(std::ios::goodbit);
        stream.clear();
        stream.rdbuf(oldBuffer);
        stream.exceptions(oldExceptions);
    }

  private:
    std::ostream& stream;
    std::streambuf* oldBuffer;
    std::ios::iostate oldExceptions;
    ThrowingStreamBuf throwingBuffer;
};

} // namespace

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_open64(const char* path, int flags, ...);
extern "C" int __real_close(int fd);

extern "C" int __wrap_open(const char* path, int flags, ...)
{
    constexpr std::string_view fakeI2cPrefix = "/dev/i2c-";
    if (std::strncmp(path, fakeI2cPrefix.data(), fakeI2cPrefix.size()) != 0)
    {
        va_list args;
        va_start(args, flags);
        const int mode = ((flags & O_CREAT) != 0) ? va_arg(args, int) : 0;
        va_end(args);
        if ((flags & O_CREAT) != 0)
        {
            return __real_open(path, flags, mode);
        }
        return __real_open(path, flags);
    }

    openedPaths.emplace_back(path);
    if (openResult < 0)
    {
        errno = ENOENT;
        return -1;
    }
    return openResult;
}

extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    constexpr std::string_view fakeI2cPrefix = "/dev/i2c-";
    if (std::strncmp(path, fakeI2cPrefix.data(), fakeI2cPrefix.size()) != 0)
    {
        va_list args;
        va_start(args, flags);
        const int mode = ((flags & O_CREAT) != 0) ? va_arg(args, int) : 0;
        va_end(args);
        if ((flags & O_CREAT) != 0)
        {
            return __real_open64(path, flags, mode);
        }
        return __real_open64(path, flags);
    }

    openedPaths.emplace_back(path);
    if (openResult < 0)
    {
        errno = ENOENT;
        return -1;
    }
    return openResult;
}

extern "C" int __wrap_close(int fd)
{
    if (fd != openResult)
    {
        return __real_close(fd);
    }
    closedFds.push_back(fd);
    return 0;
}

namespace recovery_tool::i2c_utils
{

bool sendI2cCmdForRead(int, uint16_t, std::vector<uint8_t>& commandData,
                       std::vector<uint8_t>& readData, bool)
{
    capturedReadCommands.push_back(commandData);
    if (readResults.empty())
    {
        return false;
    }
    auto behavior = readResults.front();
    readResults.pop_front();
    if (behavior.throws)
    {
        throw std::runtime_error("fake glacier read exception");
    }
    if (!behavior.success)
    {
        return false;
    }

    const auto bytesToCopy = std::min(readData.size(), behavior.data.size());
    std::copy_n(behavior.data.begin(), bytesToCopy, readData.begin());
    return true;
}

bool sendI2cCmdForWrite(int, uint16_t, std::vector<uint8_t>& writeData, bool)
{
    capturedWrites.push_back(writeData);
    if (!writeThrows.empty())
    {
        const bool shouldThrow = writeThrows.front();
        writeThrows.pop_front();
        if (shouldThrow)
        {
            throw std::runtime_error("fake glacier write exception");
        }
    }
    if (writeResults.empty())
    {
        return true;
    }
    const bool result = writeResults.front();
    writeResults.pop_front();
    return result;
}

bool sendI2cCmdForWriteRead(int, uint16_t, std::vector<uint8_t>&,
                            std::vector<uint8_t>&, bool)
{
    return false;
}

} // namespace recovery_tool::i2c_utils

namespace glacier_recovery_tool::glacier_recovery_commands
{
uint32_t crc32(const void* data, size_t size);
} // namespace glacier_recovery_tool::glacier_recovery_commands

#define main glacier_cli_main
#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_interface.cpp"
#undef main

TEST_F(GlacierRecoveryTest, CommandHelpersCoverCrcPayloadAndValidation)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(9, 0x5A, true);
    EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
    EXPECT_EQ(commands.toOffsetBytes(0x123456),
              (std::vector<uint8_t>{0x56, 0x34, 0x12}));

    const auto commandBytes = commands.getCommandBytesWithCRC32(
        RecoveryCommand::Initialization, {0xAA});
    EXPECT_EQ(commandBytes.size(), 6u);
    EXPECT_EQ(commandBytes[0],
              static_cast<uint8_t>(RecoveryCommand::Initialization));

    const auto payload =
        commands.getWriteCommandPayload({0x11, 0x22, 0x33}, 0x20);
    EXPECT_EQ(payload[0], 0x20);
    EXPECT_EQ(payload[3], 2);
    EXPECT_EQ(payload[4], 0x11);
    EXPECT_EQ(payload.size(), 39u);

    EXPECT_EQ(commands.ValidateWriteImageResponse(RecoveryCommand::HeaderWrite,
                                                  {0, 0, crcFailField}),
              RecoveryResult::CRCFailure);
    EXPECT_EQ(
        commands.ValidateWriteImageResponse(RecoveryCommand::KeyHashBlobWrite,
                                            {0, 0, illegalOffsetField}),
        RecoveryResult::IllegalKeyHashBlobOffset);
    EXPECT_EQ(
        commands.ValidateWriteImageResponse(RecoveryCommand::FWImageWrite,
                                            {0, 0, invalidCmdSignatureField}),
        RecoveryResult::InvalidCommandSignature);

    auto pending = makeResponse(RecoveryCommand::GetResponse, 8);
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              pending,
                                              ResponseLength::InitResponse),
              RecoveryResult::Pending);
    auto initOk = makeResponse(RecoveryCommand::Initialization, 8,
                               {initResponseByte1, initResponseByte2});
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              initOk,
                                              ResponseLength::InitResponse),
              RecoveryResult::Ok);

    EXPECT_EQ(
        commands.recoveryResultToStr(RecoveryResult::FirmwareNotInRecovery),
        "Device is not in Recovery.");
}

TEST_F(GlacierRecoveryTest, CommandHelpersCoverVerboseValidationAndTimeoutPaths)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(2, 0x31, true);

    openResult = -1;
    EXPECT_THROW(commands.openI2CDevice(), std::runtime_error);
    openResult = 61;

    testing::internal::CaptureStdout();
    commands.logVerbose("hello");
    commands.printBuffer(Tx, {0xAA, 0x55});
    const auto verboseOutput = testing::internal::GetCapturedStdout();
    EXPECT_NE(verboseOutput.find("hello"), std::string::npos);
    EXPECT_NE(verboseOutput.find("Tx:"), std::string::npos);

    for (auto result : {RecoveryResult::Ok,
                        RecoveryResult::IllegalPayloadLength,
                        RecoveryResult::CRCFailure,
                        RecoveryResult::ResponseCRCCompFailure,
                        RecoveryResult::IllegalHeaderOffset,
                        RecoveryResult::IllegalKeyHashBlobOffset,
                        RecoveryResult::IllegalFWImageWriteAddress,
                        RecoveryResult::InvalidCommandSignature,
                        RecoveryResult::FirmwareNotInRecovery,
                        RecoveryResult::InitResponseByteMismatch,
                        RecoveryResult::BadResponse,
                        RecoveryResult::InvalidCommand,
                        RecoveryResult::Pending,
                        RecoveryResult::FailedToReadData,
                        RecoveryResult::InvalidRevision,
                        RecoveryResult::SRAMCmdFailed,
                        RecoveryResult::FileOpenFailure,
                        RecoveryResult::FailedToReadVendorDetails,
                        RecoveryResult::FailedToReadHeader,
                        RecoveryResult::FailedToReadKHB,
                        RecoveryResult::FailedToReadFWImage,
                        static_cast<RecoveryResult>(0xFF)})
    {
        EXPECT_FALSE(commands.recoveryResultToStr(result).empty());
    }

    readResults.push_back({.success = false, .data = {}});
    EXPECT_THROW(commands.GetResponse(ResponseLength::InitResponse),
                 std::runtime_error);

    auto wrongSize = makeResponse(RecoveryCommand::Initialization, 8,
                                  {initResponseByte1, initResponseByte2});
    wrongSize.pop_back();
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              wrongSize,
                                              ResponseLength::InitResponse),
              RecoveryResult::IllegalPayloadLength);

    auto badCrc = makeResponse(RecoveryCommand::Initialization, 8,
                               {initResponseByte1, initResponseByte2});
    badCrc.back() ^= 0x1;
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              badCrc,
                                              ResponseLength::InitResponse),
              RecoveryResult::ResponseCRCCompFailure);

    auto initMismatch =
        makeResponse(RecoveryCommand::Initialization, 8, {0x00, 0x00});
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              initMismatch,
                                              ResponseLength::InitResponse),
              RecoveryResult::InitResponseByteMismatch);

    auto fwInfoBad =
        makeResponse(RecoveryCommand::GetFWInfo, 26, {0x00, 0x01, 0x02});
    EXPECT_EQ(
        commands.ValidateGetResponseCmd(RecoveryCommand::GetFWInfo, fwInfoBad,
                                        ResponseLength::FwInfoRevBResponse),
        RecoveryResult::BadResponse);

    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::HeaderWrite,
                  makeWriteStatusResponse(RecoveryCommand::HeaderWrite,
                                          illegalLengthField)),
              RecoveryResult::IllegalPayloadLength);
    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::FWImageWrite,
                  makeWriteStatusResponse(RecoveryCommand::FWImageWrite,
                                          invalidCmdSignatureField)),
              RecoveryResult::InvalidCommandSignature);
    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::HeaderWrite,
                  makeWriteStatusResponse(RecoveryCommand::HeaderWrite,
                                          illegalOffsetField)),
              RecoveryResult::IllegalHeaderOffset);
    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::KeyHashBlobWrite,
                  makeWriteStatusResponse(RecoveryCommand::KeyHashBlobWrite,
                                          illegalOffsetField)),
              RecoveryResult::IllegalKeyHashBlobOffset);
    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::FWImageWrite,
                  makeWriteStatusResponse(RecoveryCommand::FWImageWrite,
                                          illegalOffsetField)),
              RecoveryResult::IllegalFWImageWriteAddress);
    EXPECT_EQ(commands.ValidateWriteImageResponse(
                  RecoveryCommand::Initialization,
                  makeWriteStatusResponse(RecoveryCommand::Initialization,
                                          illegalOffsetField)),
              RecoveryResult::InvalidCommand);

    readResults.clear();
    for (size_t i = 0; i < maxRetries; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::GetResponse, 8)});
    }
    EXPECT_EQ(
        commands.executeGetResponseCmdAndValidateResponse(
            RecoveryCommand::Initialization, ResponseLength::InitResponse),
        RecoveryResult::Pending);
}

TEST_F(GlacierRecoveryTest,
       CommandHelpersCoverQuietLoggingZeroLengthBufferAndFwInfoErrors)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands quietCommands(2, 0x31, false);
    GlacierRecoveryCommands verboseCommands(2, 0x31, true);

    testing::internal::CaptureStdout();
    quietCommands.logVerbose("quiet");
    quietCommands.printBuffer(Tx, {0xAA, 0x55});
    verboseCommands.printBuffer(Rx, {});
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(output.empty() || output.find("Rx:") != std::string::npos);

    quietCommands.revision = Revision::RevB;
    writeResults = {true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    auto [wrongCommandResult, wrongCommandData] =
        quietCommands.getFirmwareInfoCommand();
    EXPECT_EQ(wrongCommandResult, RecoveryResult::FirmwareNotInRecovery);
    EXPECT_TRUE(wrongCommandData.empty());

    writeResults = {true};
    readResults.clear();
    auto badCrc = makeResponse(RecoveryCommand::GetFWInfo, 26,
                               {0x00, 0x50, 0x48, 0x43, 0x4D});
    badCrc.back() ^= 0x1;
    readResults.push_back({.success = true, .data = badCrc});
    auto [crcResult, crcData] = quietCommands.getFirmwareInfoCommand();
    EXPECT_EQ(crcResult, RecoveryResult::ResponseCRCCompFailure);
    EXPECT_TRUE(crcData.empty());
}

TEST_F(GlacierRecoveryTest, InitializationFirmwareInfoAndWriteImageSucceed)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(4, 0x66, false);

    writeResults = {true, true, true, true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    EXPECT_EQ(commands.performInitialization(), RecoveryResult::Ok);

    commands.revision = Revision::RevB;
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                              {0x00, 0x50, 0x48, 0x43, 0x4D})});
    auto [fwInfoResult, fwInfoData] = commands.getFirmwareInfoCommand();
    EXPECT_EQ(fwInfoResult, RecoveryResult::Ok);
    EXPECT_EQ(fwInfoData.size(), 26u);

    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::HeaderWrite, 7)});
    EXPECT_EQ(commands.executeWriteImageCmd(RecoveryCommand::HeaderWrite,
                                            std::vector<uint8_t>(32, 0xAB), 0),
              RecoveryResult::Ok);

    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::FWImageWrite, 7)});
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::FWImageWrite, 7)});
    EXPECT_EQ(commands.writeImage(RecoveryCommand::FWImageWrite, 140,
                                  std::vector<uint8_t>(140, 0x5A)),
              RecoveryResult::Ok);

    writeResults = {true};
    EXPECT_TRUE(commands.unlockI2CDevice());
    EXPECT_FALSE(openedPaths.empty());
    EXPECT_EQ(openedPaths.front(), "/dev/i2c-4");
}

TEST_F(GlacierRecoveryTest, GlacierToolAndRecoveryImageCoverSuccessAndFailure)
{
    using namespace glacier_recovery_tool;
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(8, 0x22, false);
    EXPECT_EQ(commands.performGlacierRecovery("/tmp/does-not-exist.bin"),
              RecoveryResult::FileOpenFailure);

    auto imagePath = writeGlacierImage("glacier_image.bin");
    writeResults = std::deque<bool>(15, true);
    queueGlacierWriteResponses();
    EXPECT_EQ(commands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::Ok);

    GlacierRecoveryTool tool(1, 0x20, false);
    writeResults.clear();
    readResults.clear();
    writeResults = {true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    EXPECT_EQ(tool.getRecoveryStatusJson()["Status"], "Device is in Recovery");

    writeResults.clear();
    readResults.clear();
    writeResults = {true, true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                              {0x00, 0x50, 0x48, 0x43, 0x4D, 0x01, 0x02, 0x03,
                               0x04, 0x05, 0x06, 0x07})});
    auto fwJson = tool.getFirmwareInfoJson();
    EXPECT_TRUE(fwJson.contains("Build Number"));

    writeResults.clear();
    readResults.clear();
    writeResults = {true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    EXPECT_TRUE(
        tool.performRecovery("/tmp/does-not-exist.bin").contains("Error"));

    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest, InitializationFirmwareInfoAndUtilityErrorPaths)
{
    using namespace glacier_recovery_tool;
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(7, 0x44, false);

    writeResults = {false};
    EXPECT_EQ(commands.performInitialization(),
              RecoveryResult::FailedToReadData);

    writeResults = {false};
    EXPECT_EQ(std::get<0>(commands.getFirmwareInfoCommand()),
              RecoveryResult::FailedToReadData);

    commands.revision = Revision::RevUnknown;
    writeResults = {true};
    EXPECT_EQ(std::get<0>(commands.getFirmwareInfoCommand()),
              RecoveryResult::InvalidRevision);

    commands.revision = Revision::RevA;
    writeResults = {true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetFWInfo, 18,
                              {0x00, 0x50, 0x48, 0x43, 0x4D})});
    auto [revAResult, revAData] = commands.getFirmwareInfoCommand();
    EXPECT_EQ(revAResult, RecoveryResult::Ok);
    EXPECT_EQ(revAData.size(), 18u);

    commands.revision = Revision::RevB;
    writeResults = {true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetResponse, 26)});
    readResults.push_back({.success = true,
                           .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                                                {0x00, 0x00, 0x00})});
    auto [badFwInfoResult, badFwInfoData] = commands.getFirmwareInfoCommand();
    EXPECT_EQ(badFwInfoResult, RecoveryResult::BadResponse);
    EXPECT_TRUE(badFwInfoData.empty());

    writeResults = {false};
    EXPECT_EQ(commands.executeWriteImageCmd(RecoveryCommand::HeaderWrite,
                                            {0x11, 0x22}, 0),
              RecoveryResult::FailedToReadData);

    writeResults = {true, true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::FWImageWrite, 7)});
    readResults.push_back(
        {.success = true,
         .data = makeWriteStatusResponse(RecoveryCommand::FWImageWrite,
                                         invalidCmdSignatureField)});
    EXPECT_EQ(commands.writeImage(RecoveryCommand::FWImageWrite, 140,
                                  std::vector<uint8_t>(140, 0x5A)),
              RecoveryResult::InvalidCommandSignature);

    GlacierRecoveryTool tool(1, 0x20, true);

    writeResults = {false};
    readResults.clear();
    EXPECT_TRUE(tool.getRecoveryStatusJson().contains("Error"));

    openResult = -1;
    EXPECT_TRUE(tool.getRecoveryStatusJson().contains("Error"));
    openResult = 61;

    writeResults = {true, true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    readResults.push_back({.success = true,
                           .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                                                {0x00, 0x00, 0x00})});
    EXPECT_TRUE(tool.getFirmwareInfoJson().contains("Error"));

    openResult = -1;
    EXPECT_TRUE(tool.getFirmwareInfoJson().contains("Error"));
    openResult = 61;

    writeResults = {false};
    readResults.clear();
    EXPECT_TRUE(tool.performRecovery("/tmp/missing.bin").contains("Error"));

    auto imagePath = writeGlacierImage("glacier_exception_image.bin");
    openResult = -1;
    EXPECT_TRUE(tool.performRecovery(imagePath.string()).contains("Error"));
    openResult = 61;
    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest,
       GlacierToolFirmwareInfoInitFailureAndRevARecoveryAreCovered)
{
    using namespace glacier_recovery_tool;
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryTool quietTool(1, 0x20, false);
    writeResults = {false};
    readResults.clear();
    auto initFailJson = quietTool.getFirmwareInfoJson();
    EXPECT_TRUE(initFailJson.contains("Error"));
    EXPECT_EQ(initFailJson["Error"], "Failed to read data from the device.");

    GlacierRecoveryCommands revACommands(8, 0x22, false);
    revACommands.revision = Revision::RevA;
    auto imagePath = writeGlacierImage("glacier_reva_success.bin");
    std::filesystem::resize_file(imagePath, 4096);
    writeResults = std::deque<bool>(16, true);
    readResults.clear();
    for (size_t i = 0; i < 9; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::KeyHashBlobWrite, 7)});
    }
    for (size_t i = 0; i < 7; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::HeaderWrite, 7)});
    }
    for (size_t i = 0; i < 2; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::FWImageWrite, 7)});
    }
    EXPECT_EQ(revACommands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::Ok);
    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest, CommandHelpersPropagateOpenFailures)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(5, 0x20, false);
    openResult = -1;

    EXPECT_THROW(commands.performInitialization(), std::runtime_error);
    EXPECT_THROW(std::ignore = commands.getFirmwareInfoCommand(),
                 std::runtime_error);
    EXPECT_THROW(
        commands.executeGetResponseCmdAndValidateResponse(
            RecoveryCommand::Initialization, ResponseLength::InitResponse),
        std::runtime_error);
    EXPECT_THROW(commands.executeWriteImageCmd(RecoveryCommand::HeaderWrite,
                                               {0x11, 0x22}, 0),
                 std::runtime_error);
    EXPECT_THROW(commands.performSRAMExe(), std::runtime_error);
    EXPECT_THROW(commands.unlockI2CDevice(), std::runtime_error);

    openResult = 61;
}

TEST_F(GlacierRecoveryTest,
       CommandHelpersPropagateWriteExceptionsAndVerboseOutputFailures)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands quietCommands(5, 0x20, false);

    writeThrows = {true};
    EXPECT_THROW(quietCommands.performInitialization(), std::runtime_error);

    writeThrows = {true};
    EXPECT_THROW(std::ignore = quietCommands.getFirmwareInfoCommand(),
                 std::runtime_error);

    writeThrows = {true};
    EXPECT_THROW(quietCommands.executeWriteImageCmd(
                     RecoveryCommand::HeaderWrite, {0x11, 0x22}, 0),
                 std::runtime_error);

    writeThrows = {true};
    EXPECT_THROW(quietCommands.performSRAMExe(), std::runtime_error);

    writeThrows = {true};
    EXPECT_THROW(quietCommands.unlockI2CDevice(), std::runtime_error);

    auto throwImagePath = writeGlacierImage("glacier_write_throw.bin");
    writeThrows = {true};
    EXPECT_THROW(quietCommands.performGlacierRecovery(throwImagePath.string()),
                 std::runtime_error);
    EXPECT_TRUE(std::filesystem::remove(throwImagePath));

    GlacierRecoveryCommands verboseCommands(2, 0x31, true);

    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.logVerbose("boom"), std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.printBuffer(Tx, {0xAA, 0x55}),
                     std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.GetResponse(ResponseLength::InitResponse),
                     std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.performInitialization(),
                     std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(std::ignore = verboseCommands.getFirmwareInfoCommand(),
                     std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.executeWriteImageCmd(
                         RecoveryCommand::HeaderWrite, {0x11, 0x22}, 0),
                     std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.performSRAMExe(), std::runtime_error);
    }
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(verboseCommands.unlockI2CDevice(), std::runtime_error);
    }

    auto verboseImagePath = writeGlacierImage("glacier_verbose_throw.bin");
    {
        PropagatingStreamFailureGuard guard(std::cout);
        EXPECT_THROW(
            verboseCommands.performGlacierRecovery(verboseImagePath.string()),
            std::runtime_error);
    }
    EXPECT_TRUE(std::filesystem::remove(verboseImagePath));
}

TEST_F(GlacierRecoveryTest,
       CommandHelpersCoverPendingExhaustionWrongCommandAndDirectHelpers)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(7, 0x44, false);

    EXPECT_EQ(commands.ValidateGetResponseCmd(
                  RecoveryCommand::Initialization,
                  makeResponse(RecoveryCommand::HeaderWrite, 7),
                  ResponseLength::InitResponse),
              RecoveryResult::FirmwareNotInRecovery);

    writeResults = {true};
    readResults.clear();
    for (size_t i = 0; i < maxRetries; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::GetResponse, 26)});
    }
    auto [pendingResult, pendingData] = commands.getFirmwareInfoCommand();
    EXPECT_EQ(pendingResult, RecoveryResult::Pending);
    EXPECT_TRUE(pendingData.empty());

    writeResults = {true};
    readResults.clear();
    readResults.push_back({.throws = true, .data = {}});
    EXPECT_THROW(std::ignore = commands.getFirmwareInfoCommand(),
                 std::runtime_error);

    writeResults = {true};
    readResults.clear();
    readResults.push_back({.throws = true, .data = {}});
    EXPECT_THROW(commands.GetResponse(ResponseLength::InitResponse),
                 std::runtime_error);

    EXPECT_EQ(commands.writeImage(RecoveryCommand::HeaderWrite, 0, {}),
              RecoveryResult::Ok);

    writeResults = {true};
    EXPECT_TRUE(commands.performSRAMExe());
    writeResults = {false};
    EXPECT_FALSE(commands.performSRAMExe());

    writeResults = {false};
    EXPECT_FALSE(commands.unlockI2CDevice());
}

TEST_F(GlacierRecoveryTest,
       CommandHelpersCoverDeeperInitializationAndFwInfoMismatchBranches)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(3, 0x22, false);

    auto initSecondByteMismatch = makeResponse(RecoveryCommand::Initialization,
                                               8, {initResponseByte1, 0x00});
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              initSecondByteMismatch,
                                              ResponseLength::InitResponse),
              RecoveryResult::InitResponseByteMismatch);

    auto initFirstByteMismatch = makeResponse(RecoveryCommand::Initialization,
                                              8, {0x00, initResponseByte2});
    EXPECT_EQ(commands.ValidateGetResponseCmd(RecoveryCommand::Initialization,
                                              initFirstByteMismatch,
                                              ResponseLength::InitResponse),
              RecoveryResult::InitResponseByteMismatch);

    auto fwInfoSecondCheckFails = makeResponse(RecoveryCommand::GetFWInfo, 26,
                                               {0x00, 0x50, 0x00, 0x00, 0x00});
    EXPECT_EQ(commands.ValidateGetResponseCmd(
                  RecoveryCommand::GetFWInfo, fwInfoSecondCheckFails,
                  ResponseLength::FwInfoRevBResponse),
              RecoveryResult::BadResponse);

    auto fwInfoThirdCheckFails = makeResponse(RecoveryCommand::GetFWInfo, 26,
                                              {0x00, 0x50, 0x48, 0x00, 0x00});
    EXPECT_EQ(commands.ValidateGetResponseCmd(
                  RecoveryCommand::GetFWInfo, fwInfoThirdCheckFails,
                  ResponseLength::FwInfoRevBResponse),
              RecoveryResult::BadResponse);

    auto fwInfoFourthCheckFails = makeResponse(RecoveryCommand::GetFWInfo, 26,
                                               {0x00, 0x50, 0x48, 0x43, 0x00});
    EXPECT_EQ(commands.ValidateGetResponseCmd(
                  RecoveryCommand::GetFWInfo, fwInfoFourthCheckFails,
                  ResponseLength::FwInfoRevBResponse),
              RecoveryResult::BadResponse);
}

TEST_F(GlacierRecoveryTest, PerformGlacierRecoveryFailureModes)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(8, 0x55, false);

    auto headerPath = writeGlacierImage("glacier_header_fail.bin");
    std::filesystem::resize_file(headerPath, 700);
    EXPECT_EQ(commands.performGlacierRecovery(headerPath.string()),
              RecoveryResult::FailedToReadHeader);
    EXPECT_TRUE(std::filesystem::remove(headerPath));

    auto fwPath = writeGlacierImage("glacier_fw_fail.bin");
    std::filesystem::resize_file(fwPath, 1200);
    EXPECT_EQ(commands.performGlacierRecovery(fwPath.string()),
              RecoveryResult::FailedToReadFWImage);
    EXPECT_TRUE(std::filesystem::remove(fwPath));

    auto khbPath = writeGlacierImage("glacier_khb_fail.bin");
    std::filesystem::resize_file(khbPath, 2300);
    EXPECT_EQ(commands.performGlacierRecovery(khbPath.string()),
              RecoveryResult::FailedToReadKHB);
    EXPECT_TRUE(std::filesystem::remove(khbPath));

    auto revAPath = writeGlacierImage("glacier_reva_fail.bin");
    commands.revision = Revision::RevA;
    EXPECT_EQ(commands.performGlacierRecovery(revAPath.string()),
              RecoveryResult::FailedToReadKHB);
    EXPECT_TRUE(std::filesystem::remove(revAPath));
    commands.revision = Revision::RevB;

    auto imagePath = writeGlacierImage("glacier_write_fail.bin");

    writeResults = {false};
    readResults.clear();
    EXPECT_EQ(commands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::FailedToReadData);

    writeResults = std::deque<bool>(6, true);
    writeResults[5] = false;
    queueGlacierWriteResponses();
    EXPECT_EQ(commands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::FailedToReadData);

    writeResults = std::deque<bool>(13, true);
    writeResults[12] = false;
    queueGlacierWriteResponses();
    EXPECT_EQ(commands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::FailedToReadData);

    writeResults = std::deque<bool>(15, true);
    writeResults[14] = false;
    queueGlacierWriteResponses();
    EXPECT_EQ(commands.performGlacierRecovery(imagePath.string()),
              RecoveryResult::SRAMCmdFailed);

    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest, PerformGlacierRecoveryCoversVendorAndStageFailures)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryCommands commands(8, 0x55, false);

    auto vendorPath = writeGlacierImage("glacier_vendor_fail.bin");
    std::filesystem::resize_file(vendorPath, 258);
    EXPECT_EQ(commands.performGlacierRecovery(vendorPath.string()),
              RecoveryResult::FailedToReadVendorDetails);
    EXPECT_TRUE(std::filesystem::remove(vendorPath));

    auto khbPath = writeGlacierImage("glacier_khb_status_fail.bin");
    writeResults = {true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeWriteStatusResponse(RecoveryCommand::KeyHashBlobWrite,
                                         invalidCmdSignatureField)});
    EXPECT_EQ(commands.performGlacierRecovery(khbPath.string()),
              RecoveryResult::InvalidCommandSignature);
    EXPECT_TRUE(std::filesystem::remove(khbPath));

    auto headerPath = writeGlacierImage("glacier_header_status_fail.bin");
    writeResults = std::deque<bool>(6, true);
    readResults.clear();
    for (size_t i = 0; i < 5; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::KeyHashBlobWrite, 7)});
    }
    readResults.push_back(
        {.success = true,
         .data = makeWriteStatusResponse(RecoveryCommand::HeaderWrite,
                                         illegalOffsetField)});
    EXPECT_EQ(commands.performGlacierRecovery(headerPath.string()),
              RecoveryResult::IllegalHeaderOffset);
    EXPECT_TRUE(std::filesystem::remove(headerPath));

    auto fwPath = writeGlacierImage("glacier_fw_status_fail.bin");
    writeResults = std::deque<bool>(13, true);
    readResults.clear();
    for (size_t i = 0; i < 5; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::KeyHashBlobWrite, 7)});
    }
    for (size_t i = 0; i < 7; ++i)
    {
        readResults.push_back(
            {.success = true,
             .data = makeResponse(RecoveryCommand::HeaderWrite, 7)});
    }
    readResults.push_back(
        {.success = true,
         .data = makeWriteStatusResponse(RecoveryCommand::FWImageWrite,
                                         illegalOffsetField)});
    EXPECT_EQ(commands.performGlacierRecovery(fwPath.string()),
              RecoveryResult::IllegalFWImageWriteAddress);
    EXPECT_TRUE(std::filesystem::remove(fwPath));
}

TEST_F(GlacierRecoveryTest, InterfaceMainExecutesAllSubcommands)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    glacier_recovery_tool::interface::commands.clear();

    writeResults = {true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    const char* getStatusArgv[] = {
        "glacier", "GetRecoveryStatus", "-b", "1", "-s", "32"};
    EXPECT_EQ(glacier_cli_main(6, const_cast<char**>(getStatusArgv)), 0);

    writeResults = {true, true};
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                              {0x00, 0x50, 0x48, 0x43, 0x4D})});
    const char* getInfoArgv[] = {"glacier", "GetFirmwareInfo", "-b", "1", "-s",
                                 "32"};
    EXPECT_EQ(glacier_cli_main(6, const_cast<char**>(getInfoArgv)), 0);

    auto imagePath = writeGlacierImage("glacier_cli_image.bin");
    writeResults = std::deque<bool>(16, true);
    queueGlacierWriteResponses();
    readResults.push_front(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    const char* performArgv[] = {
        "glacier", "PerformGlacierRecovery", "-b", "1", "-s", "32",
        "-i",      imagePath.c_str()};
    EXPECT_EQ(glacier_cli_main(8, const_cast<char**>(performArgv)), 0);
    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest, InterfaceMainHandlesParseFailure)
{
    glacier_recovery_tool::interface::commands.clear();
    const char* argv[] = {"glacier"};
    EXPECT_NE(glacier_cli_main(1, const_cast<char**>(argv)), 0);
}

TEST_F(GlacierRecoveryTest, InterfaceExecCatchBranchesAreCoveredByBrokenStdout)
{
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    glacier_recovery_tool::interface::commands.clear();
    writeResults = {true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    {
        StreamFailureGuard guard(std::cout);
        const char* argv[] = {"glacier", "GetRecoveryStatus", "-b", "1", "-s",
                              "32"};
        EXPECT_EQ(glacier_cli_main(6, const_cast<char**>(argv)), 0);
    }

    glacier_recovery_tool::interface::commands.clear();
    writeResults = {true, true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::GetFWInfo, 26,
                              {0x00, 0x50, 0x48, 0x43, 0x4D})});
    {
        StreamFailureGuard guard(std::cout);
        const char* argv[] = {"glacier", "GetFirmwareInfo", "-b", "1", "-s",
                              "32"};
        EXPECT_EQ(glacier_cli_main(6, const_cast<char**>(argv)), 0);
    }

    glacier_recovery_tool::interface::commands.clear();
    auto imagePath = writeGlacierImage("glacier_cli_catch_image.bin");
    writeResults = std::deque<bool>(16, true);
    readResults.clear();
    queueGlacierWriteResponses();
    readResults.push_front(
        {.success = true,
         .data = makeResponse(RecoveryCommand::Initialization, 8,
                              {initResponseByte1, initResponseByte2})});
    {
        StreamFailureGuard guard(std::cout);
        const char* argv[] = {
            "glacier", "PerformGlacierRecovery", "-b", "1", "-s", "32",
            "-i",      imagePath.c_str()};
        EXPECT_EQ(glacier_cli_main(8, const_cast<char**>(argv)), 0);
    }
    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(GlacierRecoveryTest, InterfaceMainCoversAdditionalParseFailures)
{
    glacier_recovery_tool::interface::commands.clear();
    const char* unknownArgv[] = {"glacier", "UnknownCommand"};
    EXPECT_NE(glacier_cli_main(2, const_cast<char**>(unknownArgv)), 0);

    glacier_recovery_tool::interface::commands.clear();
    const char* missingArgv[] = {"glacier", "GetFirmwareInfo", "-b", "1"};
    EXPECT_NE(glacier_cli_main(4, const_cast<char**>(missingArgv)), 0);
}

TEST_F(GlacierRecoveryTest,
       GlacierToolStatusCoversFirmwareNotInRecoverySpecificBranch)
{
    using namespace glacier_recovery_tool;
    using namespace glacier_recovery_tool::glacier_recovery_commands;

    GlacierRecoveryTool tool(1, 0x20, false);
    writeResults = {true};
    readResults.clear();
    readResults.push_back(
        {.success = true,
         .data = makeResponse(RecoveryCommand::HeaderWrite, 8)});
    auto statusJson = tool.getRecoveryStatusJson();
    EXPECT_EQ(statusJson["Status"], "Device is not in Recovery.");
}
