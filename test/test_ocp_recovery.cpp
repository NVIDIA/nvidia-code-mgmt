#include <fcntl.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#define private public
#include "../recovery_tool/ocp_recovery_tool/recovery_commands.hpp"
#include "../recovery_tool/ocp_recovery_tool/recoverytool_utils.hpp"
#undef private

#include "../recovery_tool/ocp_recovery_tool/usb_i2c_mapper.hpp"

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
int openResult = 42;

std::filesystem::path writeTempFile(std::string_view name,
                                    const std::vector<uint8_t>& bytes)
{
    const auto path =
        std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<uint8_t> makeBuffer(size_t size)
{
    return std::vector<uint8_t>(size, 0);
}

class OcpRecoveryTest : public ::testing::Test
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
        openResult = 42;
    }
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

    if ((flags & O_CREAT) != 0)
    {
        va_list args;
        va_start(args, flags);
        (void)va_arg(args, int);
        va_end(args);
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

    if ((flags & O_CREAT) != 0)
    {
        va_list args;
        va_start(args, flags);
        (void)va_arg(args, int);
        va_end(args);
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
        throw std::runtime_error("fake i2c read exception");
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
            throw std::runtime_error("fake i2c write exception");
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

TEST_F(OcpRecoveryTest, CommandsBuildExpectedI2cRequests)
{
    using recovery_tool::recovery_commands::ImageType;
    using recovery_tool::recovery_commands::OCPRecoveryCommands;
    using recovery_tool::recovery_commands::RecoveryCommands;

    OCPRecoveryCommands commands(7, 0x50, true, false);
    EXPECT_EQ(commands.constructI2CDevicePath(), "/dev/i2c-7");

    writeResults = {true, true, true};
    EXPECT_TRUE(
        commands.setRecoveryControlRegisterCommand(ImageType::CMS0, false));
    ASSERT_EQ(capturedWrites.back().size(), 5u);
    EXPECT_EQ(capturedWrites.back()[0],
              static_cast<uint8_t>(RecoveryCommands::RecoveryCtrl));
    EXPECT_EQ(capturedWrites.back()[4], 0x00);

    EXPECT_TRUE(
        commands.setRecoveryControlRegisterCommand(ImageType::CMS0, true));
    EXPECT_EQ(capturedWrites.back()[4], 0x0F);

    EXPECT_TRUE(commands.setIndirectControlRegisterCommand(ImageType::CMS1));
    ASSERT_EQ(capturedWrites.back().size(), 8u);
    EXPECT_EQ(capturedWrites.back()[0],
              static_cast<uint8_t>(RecoveryCommands::IndirectCtrl));

    EXPECT_EQ(openedPaths.front(), "/dev/i2c-7");
    EXPECT_FALSE(closedFds.empty());
}

TEST_F(OcpRecoveryTest, CommandsHandleOpenReadinessAndFileHelpers)
{
    using recovery_tool::recovery_commands::ImageType;
    using recovery_tool::recovery_commands::OCPRecoveryCommands;

    OCPRecoveryCommands commands(9, 0x60, false, false);
    auto imageFile =
        writeTempFile("ocp_read_firmware.bin", {0x01, 0x02, 0x03, 0x04});
    auto imageData = commands.readFirmwareImage(imageFile.string());
    EXPECT_EQ(imageData.size(), 4u);
    EXPECT_TRUE(commands.readFirmwareImage("/tmp/does-not-exist.bin").empty());

    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});
    EXPECT_TRUE(commands.isDeviceReadyForTx());

    readResults.push_back({.success = false, .data = {}});
    EXPECT_FALSE(commands.isDeviceReadyForTx());

    openResult = -1;
    EXPECT_THROW(commands.openI2CDevice(), std::runtime_error);
    openResult = 42;

    writeResults = {true, true};
    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});
    EXPECT_TRUE(commands.writeRecoveryChunk("image.bin", "CMS0",
                                            std::vector<uint8_t>(32, 0x5A), 0));

    writeResults = {true};
    readResults.push_back({.success = false, .data = {}});
    EXPECT_FALSE(commands.writeRecoveryChunk(
        "image.bin", "CMS0", std::vector<uint8_t>(16, 0xCC), 0));

    EXPECT_TRUE(std::filesystem::remove(imageFile));
}

TEST_F(OcpRecoveryTest, PublicCommandQueriesAndCmsLogsCoverSuccessAndFailure)
{
    using recovery_tool::recovery_commands::OCPRecoveryCommands;
    using recovery_tool::recovery_commands::ResponseLength;

    OCPRecoveryCommands commands(3, 0x44, false, false);

    auto deviceId =
        makeBuffer(static_cast<size_t>(ResponseLength::DeviceIDResLen));
    deviceId[1] = static_cast<uint8_t>(recovery_tool::DeviceId::PCI_Vendor);
    readResults.push_back({.success = true, .data = deviceId});
    auto [deviceIdOk, deviceIdBytes, deviceIdError] =
        commands.getDeviceIDCommand();
    EXPECT_TRUE(deviceIdOk);
    EXPECT_EQ(deviceIdBytes.size(),
              static_cast<size_t>(ResponseLength::DeviceIDResLen));
    EXPECT_TRUE(deviceIdError.empty());

    readResults.push_back({.success = false, .data = {}});
    EXPECT_FALSE(std::get<0>(commands.getDeviceStatusCommand()));

    auto recoveryStatus =
        makeBuffer(static_cast<size_t>(ResponseLength::RecoveryStatusResLen));
    recoveryStatus[1] = 0x31;
    readResults.push_back({.success = true, .data = recoveryStatus});
    EXPECT_TRUE(std::get<0>(commands.getRecoveryStatusCommand()));

    auto cmsChunk =
        makeBuffer(static_cast<size_t>(ResponseLength::CMSLogsChunkSize));
    cmsChunk[0] = 0xAA;
    writeResults = {true};
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    auto [cmsOk, cmsData, cmsErr] = commands.getCMSLogs(0);
    EXPECT_TRUE(cmsOk);
    EXPECT_EQ(cmsData.size(),
              static_cast<size_t>(ResponseLength::CMSLogsChunkSize) * 3u);
    EXPECT_TRUE(cmsErr.empty());

    writeResults = {false};
    auto [cmsFail, ignoredData, cmsFailErr] = commands.getCMSLogs(1);
    EXPECT_FALSE(cmsFail);
    EXPECT_TRUE(ignoredData.empty());
    EXPECT_FALSE(cmsFailErr.empty());

    auto outFile =
        std::filesystem::temp_directory_path() / "ocp_recovery_log.bin";
    auto [saveOk, saveErr] =
        commands.saveToLogFile({0xAA, 0xBB}, outFile.string());
    EXPECT_TRUE(saveOk);
    EXPECT_TRUE(saveErr.empty());
    EXPECT_TRUE(std::filesystem::remove(outFile));

    auto [saveBad, saveBadErr] =
        commands.saveToLogFile({0xAA}, "/this/path/will/not/exist/log.bin");
    EXPECT_FALSE(saveBad);
    EXPECT_FALSE(saveBadErr.empty());
}

TEST_F(OcpRecoveryTest,
       DirectCommandsCoverRetryExceptionAndIntermediateFailures)
{
    using recovery_tool::recovery_commands::OCPRecoveryCommands;

    OCPRecoveryCommands commands(6, 0x51, true, true);

    readResults.push_back({.throws = true, .data = {}});
    auto [indirectOk, indirectBytes, indirectErr] =
        commands.getIndirectStatusCommand();
    EXPECT_FALSE(indirectOk);
    EXPECT_TRUE(indirectBytes.empty());
    EXPECT_FALSE(indirectErr.empty());

    readResults.push_back({.success = true, .data = {0x00, 0x00}});
    readResults.push_back({.success = true, .data = {0x00, 0x00}});
    readResults.push_back({.success = true, .data = {0x00, 0x04}});
    EXPECT_TRUE(commands.isDeviceReadyForTx());

    for (int i = 0; i < 5; ++i)
    {
        readResults.push_back({.success = true, .data = {0x00, 0x00}});
    }
    EXPECT_FALSE(commands.isDeviceReadyForTx());

    writeThrows.push_back(true);
    auto [forceOk, forceErr] = commands.setForceRecoveryMode();
    EXPECT_FALSE(forceOk);
    EXPECT_FALSE(forceErr.empty());

    writeResults = {true};
    readResults.push_back(
        {.success = true, .data = std::vector<uint8_t>(253, 0xAA)});
    readResults.push_back({.success = false, .data = {}});
    auto [cmsOk, cmsBytes, cmsErr] = commands.getCMSLogs(2);
    EXPECT_FALSE(cmsOk);
    EXPECT_TRUE(cmsBytes.empty());
    EXPECT_FALSE(cmsErr.empty());

    auto imagePath = writeTempFile("ocp_branchy.bin", {0x10, 0x20, 0x30, 0x40});

    writeResults = {false};
    auto [recoveryCtrlOk, recoveryCtrlErr] =
        commands.performRecoveryCommand({imagePath.string()});
    EXPECT_FALSE(recoveryCtrlOk);
    EXPECT_NE(recoveryCtrlErr.find("RecoveryControlRegister failed"),
              std::string::npos);

    writeResults = {true, false};
    auto [indirectFailOk, indirectFailErr] =
        commands.performRecoveryCommand({imagePath.string()});
    EXPECT_FALSE(indirectFailOk);
    EXPECT_NE(indirectFailErr.find("IndirectControlRegister failed"),
              std::string::npos);

    writeResults = {true, true, false};
    auto [writeImageOk, writeImageErr] =
        commands.performRecoveryCommand({imagePath.string()});
    EXPECT_FALSE(writeImageOk);
    EXPECT_NE(writeImageErr.find("recovery image failed"), std::string::npos);

    writeResults = {true, true, true, false};
    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});
    auto [activateFailOk, activateFailErr] =
        commands.performRecoveryCommand({imagePath.string()});
    EXPECT_FALSE(activateFailOk);
    EXPECT_NE(activateFailErr.find("Activating"), std::string::npos);

    openResult = -1;
    auto [exceptionOk, exceptionErr] =
        commands.performRecoveryCommand({imagePath.string()});
    EXPECT_FALSE(exceptionOk);
    EXPECT_NE(exceptionErr.find("Error in Performing Recovery"),
              std::string::npos);
    openResult = 42;

    EXPECT_TRUE(std::filesystem::remove(imagePath));
}

TEST_F(OcpRecoveryTest, PerformRecoveryCommandAndToolMethodsCoverMainFlows)
{
    using recovery_tool::OCPRecoveryTool;
    using recovery_tool::recovery_commands::OCPRecoveryCommands;

    OCPRecoveryCommands commands(5, 0x33, false, false);
    EXPECT_FALSE(std::get<0>(
        commands.performRecoveryCommand({"/tmp/missing-image-for-ocp.bin"})));

    auto emptyImage = writeTempFile("ocp_empty.bin", {});
    EXPECT_FALSE(
        std::get<0>(commands.performRecoveryCommand({emptyImage.string()})));

    auto goodImage = writeTempFile("ocp_good.bin", {0x10, 0x20, 0x30, 0x40});
    writeResults = {true, true, true, true};
    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});
    auto [performOk, performErr] =
        commands.performRecoveryCommand({goodImage.string()});
    EXPECT_TRUE(performOk);
    EXPECT_TRUE(performErr.empty());

    OCPRecoveryTool tool(2, 0x55, false, false);
    EXPECT_EQ(tool.deviceIDToStr(recovery_tool::DeviceId::UUID), "UUID");
    EXPECT_EQ(tool.deviceStatusToStr(recovery_tool::DeviceStatus::RecoveryMode),
              "Recovery mode");
    EXPECT_EQ(tool.protocolErrorToStr(recovery_tool::ProtocolError::CrcError),
              "CRC Error");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(recovery_tool::RecoveryReasonCode::FR),
        "Forced Recovery");
    EXPECT_EQ(tool.recoveryStatusToStr(
                  recovery_tool::RecoveryStatus::RecoverySuccess),
              "Recovery successful");

    auto deviceIdBuffer = makeBuffer(25);
    deviceIdBuffer[1] =
        static_cast<uint8_t>(recovery_tool::DeviceId::PCI_Vendor);
    deviceIdBuffer[3] = 0x12;
    deviceIdBuffer[4] = 0x34;
    deviceIdBuffer[5] = 0x56;
    deviceIdBuffer[6] = 0x78;
    deviceIdBuffer[7] = 0x9A;
    deviceIdBuffer[8] = 0xBC;
    deviceIdBuffer[9] = 0xDE;
    deviceIdBuffer[10] = 0xF0;
    deviceIdBuffer[11] = 0x11;
    readResults.push_back({.success = true, .data = deviceIdBuffer});
    auto idJson = tool.getDeviceIDJson();
    EXPECT_EQ(idJson["Initial Descriptor Type"], "PCI Vendor");
    EXPECT_EQ(idJson["PCI Revision ID"], 0x11);

    auto badIdBuffer = makeBuffer(25);
    badIdBuffer[1] = static_cast<uint8_t>(recovery_tool::DeviceId::UUID);
    readResults.push_back({.success = true, .data = badIdBuffer});
    EXPECT_TRUE(tool.getDeviceIDJson().contains("Error"));

    auto statusBuffer = makeBuffer(25);
    statusBuffer[1] =
        static_cast<uint8_t>(recovery_tool::DeviceStatus::RecoveryMode);
    statusBuffer[2] =
        static_cast<uint8_t>(recovery_tool::ProtocolError::CrcError);
    statusBuffer[3] = static_cast<uint8_t>(
        static_cast<int>(recovery_tool::RecoveryReasonCode::FR) & 0xFF);
    statusBuffer[4] = static_cast<uint8_t>(
        static_cast<int>(recovery_tool::RecoveryReasonCode::FR) >> 8);
    statusBuffer[5] = 0x34;
    statusBuffer[6] = 0x12;
    statusBuffer[7] = 2;
    statusBuffer[8] = 0xAA;
    statusBuffer[9] = 0x55;
    readResults.push_back({.success = true, .data = statusBuffer});
    auto deviceStatusJson = tool.getDeviceStatusJson();
    EXPECT_EQ(deviceStatusJson["Device Status"], "Recovery mode");
    EXPECT_EQ(deviceStatusJson["Vendor Status Length"], 2);

    auto recoveryBuffer = makeBuffer(3);
    recoveryBuffer[1] = 0x21;
    recoveryBuffer[2] = 0x09;
    readResults.push_back({.success = true, .data = recoveryBuffer});
    auto recoveryJson = tool.getRecoveryStatusJson();
    EXPECT_EQ(recoveryJson["Device Recovery Status"],
              "Awaiting recovery image");
    EXPECT_EQ(recoveryJson["Recovery Image Index"], "2");

    writeResults = {true};
    EXPECT_EQ(tool.setForceRecoveryMode()["Status"], "Success");
    writeResults = {false};
    EXPECT_TRUE(tool.setForceRecoveryMode().contains("Error"));

    EXPECT_EQ(tool.performRecovery({})["Status"], "Failed");

    auto statusForRecovery = makeBuffer(25);
    statusForRecovery[1] =
        static_cast<uint8_t>(recovery_tool::DeviceStatus::RecoveryMode);
    statusForRecovery[2] =
        static_cast<uint8_t>(recovery_tool::ProtocolError::NoProtocolError);
    auto recoveryReady = makeBuffer(3);
    recoveryReady[1] = 0x01;
    readResults.push_back({.success = true, .data = statusForRecovery});
    readResults.push_back({.success = true, .data = recoveryReady});
    writeResults = {true, true, true, true};
    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});
    auto performJson = tool.performRecovery({goodImage.string()});
    EXPECT_EQ(performJson["Status"], "Successful");

    writeResults = {true};
    readResults.push_back(
        {.success = true, .data = std::vector<uint8_t>(253, 0xFE)});
    readResults.push_back(
        {.success = true, .data = std::vector<uint8_t>(253, 0xED)});
    readResults.push_back(
        {.success = true, .data = std::vector<uint8_t>(253, 0xDC)});
    auto cmsOut =
        std::filesystem::temp_directory_path() / "ocp_tool_cms_log.bin";
    EXPECT_EQ(tool.processCMSLogs(cmsOut.string(), 0)["Status"], "Successful");
    EXPECT_TRUE(std::filesystem::remove(cmsOut));

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort(
                  "missing-usb-port-for-test", true),
              -1);

    EXPECT_TRUE(std::filesystem::remove(emptyImage));
    EXPECT_TRUE(std::filesystem::remove(goodImage));
}

TEST_F(OcpRecoveryTest, ToolEnumConvertersCoverRemainingBranches)
{
    using recovery_tool::DeviceId;
    using recovery_tool::DeviceStatus;
    using recovery_tool::OCPRecoveryTool;
    using recovery_tool::ProtocolError;
    using recovery_tool::RecoveryReasonCode;
    using recovery_tool::RecoveryStatus;

    OCPRecoveryTool tool(2, 0x55, false, false);

    EXPECT_EQ(tool.deviceIDToStr(DeviceId::PCI_Vendor), "PCI Vendor");
    EXPECT_EQ(tool.deviceIDToStr(DeviceId::IANA), "IANA");
    EXPECT_EQ(tool.deviceIDToStr(DeviceId::PnP_Vendor), "PnP Vendor");
    EXPECT_EQ(tool.deviceIDToStr(DeviceId::ACPI_Vendor), "ACPI Vendor");
    EXPECT_EQ(tool.deviceIDToStr(DeviceId::IANA_Enterprise_Type),
              "IANA Enterprise Type");
    EXPECT_EQ(tool.deviceIDToStr(DeviceId::NVMe_MI), "NVMe MI");
    EXPECT_EQ(tool.deviceIDToStr(static_cast<DeviceId>(0x7E)),
              "Reserved/Unknown");

    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::StatusPending),
              "Status Pending");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::DeviceHealthy),
              "Device healthy");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::DeviceError),
              "Device Error");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::RecoveryPending),
              "Recovery Pending");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::RecoveryImgRunning),
              "Running Recovery Image");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::BootFailure),
              "Boot Failure");
    EXPECT_EQ(tool.deviceStatusToStr(DeviceStatus::FatalError), "Fatal Error");
    EXPECT_EQ(tool.deviceStatusToStr(static_cast<DeviceStatus>(0x6)),
              "Reserved/Unknown");

    EXPECT_EQ(tool.protocolErrorToStr(ProtocolError::NoProtocolError),
              "No Protocol Error");
    EXPECT_EQ(tool.protocolErrorToStr(ProtocolError::UnsupportedWriteCommand),
              "Unsupported/Write Command");
    EXPECT_EQ(tool.protocolErrorToStr(ProtocolError::UnsupportedParameter),
              "Unsupported Parameter");
    EXPECT_EQ(tool.protocolErrorToStr(ProtocolError::LengthWriteError),
              "Length write error");
    EXPECT_EQ(tool.protocolErrorToStr(ProtocolError::GeneralProtocolError),
              "General Protocol Error");
    EXPECT_EQ(tool.protocolErrorToStr(static_cast<ProtocolError>(0x20)),
              "Reserved/Unknown");

    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFNF),
              "No Boot Failure detected");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFGHWE),
              "Generic hardware error");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFGSE),
              "Generic hardware soft error - soft error may be recoverable");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFSTF),
        "Self-test failure (e.g., RSA self test failure, FIPs self test failure,, etc.)");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFCD),
              "Corrupted/missing critical data");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFKMMC),
              "Missing/corrupt key manifest");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFKMAF),
              "Authentication Failure on key manifest");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFKIAR),
              "Anti-rollback failure on key manifest");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFFIMC),
        "Missing/corrupt boot loader (first mutable code) firmware image");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFFIAF),
        "Authentication failure on boot loader (1st mutable code) firmware image");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFFIAR),
        "Anti-rollback failure boot loader (1st mutable code) firmware image");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFMFMC),
              "Missing/corrupt main/management firmware image");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFMFAF),
              "Authentication Failure main/management firmware image");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFMFAR),
              "Anti-rollback Failure main/management firmware image");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFRFMC),
              "Missing/corrupt recovery firmware");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFRFAF),
              "Authentication Failure recovery firmware");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::BFRFAR),
              "Anti-rollback Failure on recovery firmware");
    EXPECT_EQ(tool.recoveryReasonCodeToStr(RecoveryReasonCode::ReservedStart),
              "Reserved");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(RecoveryReasonCode::VendorUniqueStart),
        "Vendor Unique Boot Failure Code");
    EXPECT_EQ(
        tool.recoveryReasonCodeToStr(static_cast<RecoveryReasonCode>(0x101)),
        "Unknown");

    EXPECT_EQ(tool.recoveryStatusToStr(RecoveryStatus::NotInRecoveryMode),
              "Not in recovery mode");
    EXPECT_EQ(tool.recoveryStatusToStr(RecoveryStatus::BootingRecoveryImg),
              "Booting recovery image");
    EXPECT_EQ(tool.recoveryStatusToStr(RecoveryStatus::RecoveryFailed),
              "Recovery failed");
    EXPECT_EQ(tool.recoveryStatusToStr(RecoveryStatus::RecoveryImgAuthFailed),
              "Recovery image authentication error");
    EXPECT_EQ(
        tool.recoveryStatusToStr(RecoveryStatus::ErrorEnteringRecoveryMode),
        "Error entering Recovery mode (might be administratively disabled)");
    EXPECT_EQ(tool.recoveryStatusToStr(RecoveryStatus::InvalidCms),
              "Invalid component address space");
    EXPECT_EQ(tool.recoveryStatusToStr(static_cast<RecoveryStatus>(0x9)),
              "Reserved");
}

TEST_F(OcpRecoveryTest, ToolJsonAndRecoveryFailurePathsAreCovered)
{
    using recovery_tool::DeviceStatus;
    using recovery_tool::OCPRecoveryTool;
    using recovery_tool::ProtocolError;

    OCPRecoveryTool tool(4, 0x66, false, false);

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(tool.getDeviceIDJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(tool.getDeviceIDJson().contains("Error"));

    openResult = -1;
    EXPECT_TRUE(tool.setForceRecoveryMode().contains("Error"));
    openResult = 42;

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(tool.getDeviceStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(tool.getDeviceStatusJson().contains("Error"));

    auto statusNoVendor = makeBuffer(16);
    statusNoVendor[1] = static_cast<uint8_t>(DeviceStatus::DeviceHealthy);
    statusNoVendor[2] = static_cast<uint8_t>(ProtocolError::NoProtocolError);
    statusNoVendor[7] = 0;
    readResults.push_back({.success = true, .data = statusNoVendor});
    auto noVendorJson = tool.getDeviceStatusJson();
    EXPECT_EQ(noVendorJson["Vendor Status Length"], 0);
    EXPECT_FALSE(noVendorJson.contains("Vendor Status(in hex)"));

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(tool.getRecoveryStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(tool.getRecoveryStatusJson().contains("Error"));

    readResults.push_back({.success = false, .data = {}});
    auto statusFailJson = tool.performRecovery({"/tmp/firmware.bin"});
    EXPECT_EQ(statusFailJson["Status"], "Failed");
    EXPECT_EQ(statusFailJson["Error"], "Getting Device Status Failed");

    auto healthyStatus = makeBuffer(16);
    healthyStatus[1] = static_cast<uint8_t>(DeviceStatus::DeviceHealthy);
    healthyStatus[2] = static_cast<uint8_t>(ProtocolError::NoProtocolError);
    readResults.push_back({.success = true, .data = healthyStatus});
    auto notRecoveryJson = tool.performRecovery({"/tmp/firmware.bin"});
    EXPECT_EQ(notRecoveryJson["Status"], "Failed");
    EXPECT_EQ(notRecoveryJson["Error"], "Device is not in recovery mode");

    auto recoveryModeStatus = makeBuffer(16);
    recoveryModeStatus[1] = static_cast<uint8_t>(DeviceStatus::RecoveryMode);
    recoveryModeStatus[2] =
        static_cast<uint8_t>(ProtocolError::NoProtocolError);
    readResults.push_back({.success = true, .data = recoveryModeStatus});
    readResults.push_back({.success = false, .data = {}});
    auto recoveryStatusFailJson = tool.performRecovery({"/tmp/firmware.bin"});
    EXPECT_EQ(recoveryStatusFailJson["Status"], "Failed");
    EXPECT_EQ(recoveryStatusFailJson["Error"],
              "Getting Recovery Status Failed");

    auto notAwaiting = makeBuffer(3);
    notAwaiting[1] = 0x03;
    readResults.push_back({.success = true, .data = recoveryModeStatus});
    readResults.push_back({.success = true, .data = notAwaiting});
    auto notReadyJson = tool.performRecovery({"/tmp/firmware.bin"});
    EXPECT_EQ(notReadyJson["Status"], "Failed");
    EXPECT_EQ(notReadyJson["Error"],
              "Device is not ready to receive recovery images");

    auto awaiting = makeBuffer(3);
    awaiting[1] = 0x01;
    readResults.push_back({.success = true, .data = recoveryModeStatus});
    readResults.push_back({.success = true, .data = awaiting});
    auto commandFailJson = tool.performRecovery({"/tmp/missing-ocp-image.bin"});
    EXPECT_EQ(commandFailJson["Status"], "Failed");
    EXPECT_TRUE(commandFailJson.contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    auto exceptionJson = tool.performRecovery({"/tmp/firmware.bin"});
    EXPECT_EQ(exceptionJson["Status"], "Failed");
    EXPECT_TRUE(exceptionJson.contains("Error"));

    writeResults = {false};
    auto cmsFailJson = tool.processCMSLogs("/tmp/ocp_cms_fail.bin", 0);
    EXPECT_EQ(cmsFailJson["Status"], "Failed");
    EXPECT_TRUE(cmsFailJson.contains("Error"));

    writeResults = {true};
    readResults.push_back({.throws = true, .data = {}});
    auto cmsExceptionJson = tool.processCMSLogs("/tmp/ocp_cms_exc.bin", 0);
    EXPECT_EQ(cmsExceptionJson["Status"], "Failed");
    EXPECT_TRUE(cmsExceptionJson.contains("Error"));

    auto cmsChunk = makeBuffer(253);
    writeResults = {true};
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    auto cmsSaveFailJson =
        tool.processCMSLogs("/this/path/will/not/exist/ocp.log", 0);
    EXPECT_EQ(cmsSaveFailJson["Status"], "Failed");
    EXPECT_TRUE(cmsSaveFailJson.contains("Error"));
}

TEST_F(OcpRecoveryTest, VerboseToolMethodsCoverLogVerboseTrueBranches)
{
    using recovery_tool::OCPRecoveryTool;

    OCPRecoveryTool verboseTool(8, 0x52, true, false);

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(verboseTool.getDeviceIDJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(verboseTool.getDeviceIDJson().contains("Error"));

    writeResults = {true};
    EXPECT_EQ(verboseTool.setForceRecoveryMode()["Status"], "Success");

    writeThrows.push_back(true);
    EXPECT_TRUE(verboseTool.setForceRecoveryMode().contains("Error"));

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(verboseTool.getDeviceStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(verboseTool.getDeviceStatusJson().contains("Error"));

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(verboseTool.getRecoveryStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(verboseTool.getRecoveryStatusJson().contains("Error"));

    EXPECT_EQ(verboseTool.performRecovery({})["Status"], "Failed");

    writeResults = {false};
    auto cmsFail = verboseTool.processCMSLogs("/tmp/verbose-ocp-cms.bin", 0);
    EXPECT_EQ(cmsFail["Status"], "Failed");

    auto cmsChunk = makeBuffer(253);
    writeResults = {true};
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    auto cmsSuccess = verboseTool.processCMSLogs(
        (std::filesystem::temp_directory_path() / "verbose-ocp.log").string(),
        0);
    EXPECT_EQ(cmsSuccess["Status"], "Successful");
    std::filesystem::remove(std::filesystem::temp_directory_path() /
                            "verbose-ocp.log");
}

TEST_F(OcpRecoveryTest, NonVerboseToolMethodsCoverLogVerboseFalseBranches)
{
    using recovery_tool::OCPRecoveryTool;

    OCPRecoveryTool quietTool(8, 0x52, false, false);

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(quietTool.getDeviceIDJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(quietTool.getDeviceIDJson().contains("Error"));

    writeResults = {true};
    EXPECT_EQ(quietTool.setForceRecoveryMode()["Status"], "Success");

    writeThrows.push_back(true);
    EXPECT_TRUE(quietTool.setForceRecoveryMode().contains("Error"));

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(quietTool.getDeviceStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(quietTool.getDeviceStatusJson().contains("Error"));

    readResults.push_back({.success = false, .data = {}});
    EXPECT_TRUE(quietTool.getRecoveryStatusJson().contains("Error"));

    readResults.push_back({.throws = true, .data = {}});
    EXPECT_TRUE(quietTool.getRecoveryStatusJson().contains("Error"));

    EXPECT_EQ(quietTool.performRecovery({})["Status"], "Failed");

    writeResults = {false};
    auto cmsFail = quietTool.processCMSLogs("/tmp/quiet-ocp-cms.bin", 0);
    EXPECT_EQ(cmsFail["Status"], "Failed");

    auto cmsChunk = makeBuffer(253);
    writeResults = {true};
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    readResults.push_back({.success = true, .data = cmsChunk});
    auto quietLog =
        (std::filesystem::temp_directory_path() / "quiet-ocp.log").string();
    auto cmsSuccess = quietTool.processCMSLogs(quietLog, 0);
    EXPECT_EQ(cmsSuccess["Status"], "Successful");
    std::filesystem::remove(quietLog);
}

TEST_F(OcpRecoveryTest, DirectLogVerboseCallsCoverRemainingTemplateBranches)
{
    using recovery_tool::OCPRecoveryTool;

    constexpr char s18[] = "12345678901234567";
    constexpr char s22[] = "123456789012345678901";
    constexpr char s26[] = "1234567890123456789012345";
    constexpr char s27[] = "12345678901234567890123456";
    constexpr char s28[] = "123456789012345678901234567";
    constexpr char s29[] = "1234567890123456789012345678";
    constexpr char s32[] = "1234567890123456789012345678901";
    constexpr char s34[] = "123456789012345678901234567890123";
    constexpr char s35[] = "1234567890123456789012345678901234";
    constexpr char s36[] = "12345678901234567890123456789012345";
    constexpr char s38[] = "1234567890123456789012345678901234567";
    constexpr char s46[] = "123456789012345678901234567890123456789012345";
    constexpr char s47[] = "1234567890123456789012345678901234567890123456";
    constexpr char s55[] =
        "123456789012345678901234567890123456789012345678901234";
    constexpr char s56[] =
        "1234567890123456789012345678901234567890123456789012345";
    static_assert(sizeof(s18) == 18);
    static_assert(sizeof(s22) == 22);
    static_assert(sizeof(s26) == 26);
    static_assert(sizeof(s27) == 27);
    static_assert(sizeof(s28) == 28);
    static_assert(sizeof(s29) == 29);
    static_assert(sizeof(s32) == 32);
    static_assert(sizeof(s34) == 34);
    static_assert(sizeof(s35) == 35);
    static_assert(sizeof(s36) == 36);
    static_assert(sizeof(s38) == 38);
    static_assert(sizeof(s46) == 46);
    static_assert(sizeof(s47) == 47);
    static_assert(sizeof(s55) == 55);
    static_assert(sizeof(s56) == 56);

    OCPRecoveryTool quietTool(1, 0x20, false, false);
    OCPRecoveryTool verboseTool(1, 0x20, true, false);
    const char* ptr = "ptr";
    const char* ptrRvalue = "ptr-rvalue";
    std::string owned = "owned";

    quietTool.logVerbose(s18);
    quietTool.logVerbose(s22);
    quietTool.logVerbose(s26);
    quietTool.logVerbose(s27, ptr);
    quietTool.logVerbose(s27, static_cast<const char*>(ptrRvalue));
    quietTool.logVerbose(s28, ptr);
    quietTool.logVerbose(s28, static_cast<const char*>(ptrRvalue));
    quietTool.logVerbose(s29, ptr);
    quietTool.logVerbose(s29, static_cast<const char*>(ptrRvalue));
    quietTool.logVerbose(s32, owned);
    quietTool.logVerbose(s32);
    quietTool.logVerbose(s34);
    quietTool.logVerbose(s35);
    quietTool.logVerbose(s36, ptr);
    quietTool.logVerbose(s36, static_cast<const char*>(ptrRvalue));
    quietTool.logVerbose(s36, owned);
    quietTool.logVerbose(s38, owned);
    quietTool.logVerbose(s46, ptr);
    quietTool.logVerbose(s46, static_cast<const char*>(ptrRvalue));
    quietTool.logVerbose(s47);
    quietTool.logVerbose(s55);
    quietTool.logVerbose(s56);
    quietTool.logVerbose(owned);

    testing::internal::CaptureStdout();
    verboseTool.logVerbose(s18);
    verboseTool.logVerbose(s22);
    verboseTool.logVerbose(s26);
    verboseTool.logVerbose(s27, ptr);
    verboseTool.logVerbose(s27, static_cast<const char*>(ptrRvalue));
    verboseTool.logVerbose(s28, ptr);
    verboseTool.logVerbose(s28, static_cast<const char*>(ptrRvalue));
    verboseTool.logVerbose(s29, ptr);
    verboseTool.logVerbose(s29, static_cast<const char*>(ptrRvalue));
    verboseTool.logVerbose(s32, owned);
    verboseTool.logVerbose(s32);
    verboseTool.logVerbose(s34);
    verboseTool.logVerbose(s35);
    verboseTool.logVerbose(s36, ptr);
    verboseTool.logVerbose(s36, static_cast<const char*>(ptrRvalue));
    verboseTool.logVerbose(s36, owned);
    verboseTool.logVerbose(s38, owned);
    verboseTool.logVerbose(s46, ptr);
    verboseTool.logVerbose(s46, static_cast<const char*>(ptrRvalue));
    verboseTool.logVerbose(s47);
    verboseTool.logVerbose(s55);
    verboseTool.logVerbose(s56);
    verboseTool.logVerbose(owned);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(output.empty());
}

TEST_F(OcpRecoveryTest, EmulatedPerformRecoveryCoversSleepBranches)
{
    using recovery_tool::DeviceStatus;
    using recovery_tool::OCPRecoveryTool;
    using recovery_tool::ProtocolError;

    OCPRecoveryTool emulTool(2, 0x55, false, true);
    auto goodImage =
        writeTempFile("ocp_emul_good.bin", {0x10, 0x20, 0x30, 0x40});

    auto recoveryModeStatus = makeBuffer(16);
    recoveryModeStatus[1] = static_cast<uint8_t>(DeviceStatus::RecoveryMode);
    recoveryModeStatus[2] =
        static_cast<uint8_t>(ProtocolError::NoProtocolError);
    auto awaiting = makeBuffer(3);
    awaiting[1] = 0x01;

    readResults.push_back({.success = true, .data = recoveryModeStatus});
    readResults.push_back({.success = true, .data = awaiting});
    writeResults = {true, true, true, true};
    readResults.push_back(
        {.success = true, .data = {0x00, 0x04, 0, 0, 0, 0, 0}});

    auto performJson = emulTool.performRecovery({goodImage.string()});
    EXPECT_EQ(performJson["Status"], "Successful");

    EXPECT_TRUE(std::filesystem::remove(goodImage));
}

TEST_F(OcpRecoveryTest, ProcessCmsLogsHandlesOpenFailuresThroughOuterCatch)
{
    using recovery_tool::OCPRecoveryTool;

    OCPRecoveryTool tool(4, 0x66, false, false);
    openResult = -1;

    auto cmsJson = tool.processCMSLogs("/tmp/ocp-cms-open-fail.bin", 0);
    EXPECT_EQ(cmsJson["Status"], "Failed");
    EXPECT_NE(cmsJson["Error"].get<std::string>().find(
                  "Exception while fetching CMS logs"),
              std::string::npos);

    openResult = 42;
}
