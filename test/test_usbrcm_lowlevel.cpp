#include <libusb-1.0/libusb.h>
#include <sys/wait.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{

struct SystemExpectation
{
    std::string needle;
    int status;
};

struct ControlTransferResult
{
    int ret = LIBUSB_ERROR_IO;
    std::vector<unsigned char> data;
};

std::deque<SystemExpectation> systemExpectations;
std::vector<std::string> systemCommands;
std::map<uint32_t, ControlTransferResult> controlTransferResults;
libusb_device* fakeDevice = reinterpret_cast<libusb_device*>(0x1234);
int deviceDescriptorResult = LIBUSB_SUCCESS;
libusb_device_descriptor deviceDescriptor{};
std::string serialDescriptor;
int serialDescriptorResult = -999;

constexpr int serialResultSentinel = -999;

std::array<unsigned char, 4> littleEndian32(uint32_t value)
{
    return {static_cast<unsigned char>(value & 0xFF),
            static_cast<unsigned char>((value >> 8) & 0xFF),
            static_cast<unsigned char>((value >> 16) & 0xFF),
            static_cast<unsigned char>((value >> 24) & 0xFF)};
}

void setRegister(uint32_t address, uint32_t value)
{
    auto bytes = littleEndian32(value);
    controlTransferResults[address] = {
        .ret = static_cast<int>(bytes.size()),
        .data = std::vector<unsigned char>(bytes.begin(), bytes.end())};
}

void setTransferResult(uint32_t address, int ret,
                       std::vector<unsigned char> data = {})
{
    controlTransferResults[address] = {.ret = ret, .data = std::move(data)};
}

void pushSystemStatus(std::string_view needle, int status)
{
    systemExpectations.push_back({std::string(needle), status});
}

class UsbRcmLowLevelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        systemExpectations.clear();
        systemCommands.clear();
        controlTransferResults.clear();
        fakeDevice = reinterpret_cast<libusb_device*>(0x1234);
        deviceDescriptorResult = LIBUSB_SUCCESS;
        deviceDescriptor = {};
        deviceDescriptor.iSerialNumber = 1;
        serialDescriptor.clear();
        serialDescriptorResult = serialResultSentinel;
    }
};

} // namespace

extern "C" int __wrap_system(const char* command)
{
    systemCommands.emplace_back(command);

    if (systemExpectations.empty())
    {
        return 0 << 8;
    }

    auto expectation = systemExpectations.front();
    systemExpectations.pop_front();
    if (!expectation.needle.empty())
    {
        EXPECT_NE(systemCommands.back().find(expectation.needle),
                  std::string::npos);
    }
    return expectation.status;
}

extern "C" int __wrap_libusb_control_transfer(libusb_device_handle*, uint8_t,
                                              uint8_t, uint16_t wValue,
                                              uint16_t wIndex,
                                              unsigned char* data,
                                              uint16_t wLength, unsigned int)
{
    const uint32_t address =
        (static_cast<uint32_t>(wValue) << 16) | static_cast<uint32_t>(wIndex);

    auto it = controlTransferResults.find(address);
    if (it == controlTransferResults.end())
    {
        return LIBUSB_ERROR_IO;
    }

    const auto& result = it->second;
    if (result.ret > 0 && data != nullptr && !result.data.empty())
    {
        const auto bytesToCopy = static_cast<size_t>(
            std::min<int>(result.ret, static_cast<int>(wLength)));
        std::memcpy(data, result.data.data(), bytesToCopy);
    }
    return result.ret;
}

extern "C" libusb_device* __wrap_libusb_get_device(libusb_device_handle*)
{
    return fakeDevice;
}

extern "C" int
    __wrap_libusb_get_device_descriptor(libusb_device*,
                                        libusb_device_descriptor* desc)
{
    if (deviceDescriptorResult == LIBUSB_SUCCESS && desc != nullptr)
    {
        *desc = deviceDescriptor;
    }
    return deviceDescriptorResult;
}

extern "C" int __wrap_libusb_get_string_descriptor_ascii(libusb_device_handle*,
                                                         uint8_t,
                                                         unsigned char* data,
                                                         int length)
{
    if (serialDescriptorResult != serialResultSentinel)
    {
        return serialDescriptorResult;
    }

    const auto bytesToCopy =
        std::min<int>(length, static_cast<int>(serialDescriptor.size()));
    if (bytesToCopy > 0)
    {
        std::memcpy(data, serialDescriptor.data(),
                    static_cast<size_t>(bytesToCopy));
    }
    return bytesToCopy;
}

extern "C" const char* __wrap_libusb_error_name(int)
{
    return "WRAPPED_LIBUSB_ERROR";
}

extern "C" const char* __wrap_libusb_strerror(enum libusb_error)
{
    return "wrapped libusb strerror";
}

#include "../recovery_tool/usbrcm_recovery_tool/force_recovery.cpp"
#include "../recovery_tool/usbrcm_recovery_tool/progress_code_queue.cpp"
#include "../recovery_tool/usbrcm_recovery_tool/usb_io.cpp"

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHelpersCoverSupportedConfigs)
{
    using GpioConfig::ConfigType;

    EXPECT_EQ(GpioConfig::parseConfigType("C2"), ConfigType::C2);
    EXPECT_EQ(GpioConfig::parseConfigType("c1G2"), ConfigType::C1G2);
    EXPECT_EQ(GpioConfig::parseConfigType("C2g4"), ConfigType::C2G4);
    EXPECT_EQ(GpioConfig::parseConfigType("bad"), std::nullopt);

    EXPECT_EQ(GpioConfig::getGpioPrefix(ConfigType::C2, 0), "B0_M1_CPU");
    EXPECT_EQ(GpioConfig::getGpioPrefix(ConfigType::C2G4, 1), "BRD1_CPU");
    EXPECT_EQ(GpioConfig::getResetPrefix(ConfigType::C2, 0), "B0_M1");
    EXPECT_EQ(GpioConfig::getResetPrefix(ConfigType::C1G2, 0), "BRD0");
    EXPECT_EQ(GpioConfig::getBoardCount(ConfigType::C2), 1);
    EXPECT_EQ(GpioConfig::getBoardCount(ConfigType::C2G4), 2);

    const auto gpioSequence =
        GpioConfig::getGpioConfigSequence(ConfigType::C2, 0);
    ASSERT_EQ(gpioSequence.size(), 6u);
    EXPECT_EQ(gpioSequence.front().pinName, "B0_M1_CPU_FORCED_RECOVERY_L-O");
    EXPECT_EQ(gpioSequence.back().value, 1);

    const auto defaultSequence =
        GpioConfig::getDefaultPinStatesConfigSequence(ConfigType::C1G2, 0);
    ASSERT_EQ(defaultSequence.size(), 6u);
    EXPECT_EQ(defaultSequence.front().value, 1);
    EXPECT_EQ(GpioConfig::getResetAssert(ConfigType::C2G4, 1).pinName,
              "BRD1_IST_SYS_RST_L-O");
    EXPECT_EQ(GpioConfig::getResetRelease(ConfigType::C2, 0).value, 1);
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHelperDefaultsCoverInvalidEnumBranches)
{
    const auto invalidType = static_cast<GpioConfig::ConfigType>(0x7F);

    EXPECT_EQ(GpioConfig::getGpioPrefix(invalidType, 0), "");
    EXPECT_EQ(GpioConfig::getResetPrefix(invalidType, 1), "");
    EXPECT_EQ(GpioConfig::getBoardCount(invalidType), 0);

    const auto invalidConfig =
        GpioConfig::getGpioConfigSequence(invalidType, 0);
    ASSERT_EQ(invalidConfig.size(), 6u);
    EXPECT_EQ(invalidConfig.front().pinName, "_FORCED_RECOVERY_L-O");

    const auto invalidDefaults =
        GpioConfig::getDefaultPinStatesConfigSequence(invalidType, 1);
    ASSERT_EQ(invalidDefaults.size(), 6u);
    EXPECT_EQ(invalidDefaults.front().pinName, "_FORCED_RECOVERY_L-O");
}

TEST_F(UsbRcmLowLevelTest, ExecuteCommandAndSetGpioHandleSuccessAndFailure)
{
    EXPECT_FALSE(isValidPinName(""));
    EXPECT_FALSE(isValidPinName("CPU;reboot"));
    EXPECT_TRUE(isValidPinName("BRD0_CPU_RECOVERY_TYPE1-O"));

    pushSystemStatus("timeout 5s echo test", 0 << 8);
    EXPECT_TRUE(executeCommand("echo test"));

    pushSystemStatus("timeout 5s echo fail", SIGTERM);
    EXPECT_FALSE(executeCommand("echo fail"));

    pushSystemStatus("gpiofind BRD0_CPU_BOOT_DEV_SEL0-O", 0 << 8);
    EXPECT_TRUE(setGpio("BRD0_CPU_BOOT_DEV_SEL0-O", 1));
    ASSERT_FALSE(systemCommands.empty());
    EXPECT_NE(systemCommands.back().find(
                  "gpioset `gpiofind BRD0_CPU_BOOT_DEV_SEL0-O`=1"),
              std::string::npos);

    EXPECT_FALSE(setGpio("bad pin", 0));
}

TEST_F(UsbRcmLowLevelTest,
       ForceRecoveryBoardInstanceHandlesResetAndReleaseFailures)
{
    nlohmann::json jsonOutput;

    pushSystemStatus("B0_M1_IST_SYS_RST_L-O", 1 << 8);
    forceRecoveryMode("c2", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Error"],
              "Failed to assert reset (pin B0_M1_IST_SYS_RST_L-O)");

    jsonOutput.clear();
    pushSystemStatus("B0_M1_IST_SYS_RST_L-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_FORCED_RECOVERY_L-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_BOOT_DEV_SEL0-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_BOOT_DEV_SEL1-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_BOOT_DEV_SEL2-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_RECOVERY_TYPE0-O", 0 << 8);
    pushSystemStatus("B0_M1_CPU_RECOVERY_TYPE1-O", 0 << 8);
    pushSystemStatus("B0_M1_IST_SYS_RST_L-O", 1 << 8);
    forceRecoveryMode("c2", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Error"],
              "Failed to release reset (pin B0_M1_IST_SYS_RST_L-O)");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryAndDefaultPinsHandleTopLevelStatuses)
{
    nlohmann::json jsonOutput;

    forceRecoveryMode("unsupported", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Error"],
              "Invalid config type. Supported: c2, c1g2, c2g4");

    for (int i = 0; i < 16; ++i)
    {
        pushSystemStatus("", 0 << 8);
    }
    forceRecoveryMode("c2g4", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Successful");
    EXPECT_EQ(jsonOutput["Board0"]["Status"], "Successful");
    EXPECT_EQ(jsonOutput["Board1"]["Status"], "Successful");
    EXPECT_EQ(systemCommands.size(), 16u);

    jsonOutput.clear();
    pushSystemStatus("B0_M1_CPU_FORCED_RECOVERY_L-O", 1 << 8);
    setGPIODefaultPinStates("c2", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Error"], "Deassert CPU forced recovery");
}

TEST_F(UsbRcmLowLevelTest,
       ForceRecoveryCoversPartialBoardFailureAndDefaultStateSuccess)
{
    nlohmann::json jsonOutput;

    pushSystemStatus("BRD0_IST_SYS_RST_L-O", 0 << 8);
    pushSystemStatus("BRD1_IST_SYS_RST_L-O", 0 << 8);
    pushSystemStatus("BRD0_CPU_FORCED_RECOVERY_L-O", 1 << 8);
    for (int i = 0; i < 6; ++i)
    {
        pushSystemStatus("", 0 << 8);
    }

    forceRecoveryMode("c2g4", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Status"], "Failed");
    EXPECT_EQ(jsonOutput["Board0"]["Error"], "Assert CPU forced recovery");
    EXPECT_EQ(jsonOutput["Board1"]["Status"], "Successful");

    jsonOutput.clear();
    for (int i = 0; i < 12; ++i)
    {
        pushSystemStatus("", 0 << 8);
    }
    setGPIODefaultPinStates("c2g4", jsonOutput);
    EXPECT_EQ(jsonOutput["Status"], "Successful");
    EXPECT_EQ(jsonOutput["Board0"]["Status"], "Successful");
    EXPECT_EQ(jsonOutput["Board1"]["Status"], "Successful");
}

TEST_F(UsbRcmLowLevelTest, UsbIoReadRegister32CoversSuccessAndFailure)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x55);

    EXPECT_EQ(usb_io::readRegister32(nullptr, 0x8000, true), std::nullopt);

    setRegister(0x81234567, 0x12345678);
    auto value = usb_io::readRegister32(handle, 0x81234567, false);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x12345678u);

    setTransferResult(0x00002000, 2, {0x01, 0x02});
    EXPECT_EQ(usb_io::readRegister32(handle, 0x2000, true), std::nullopt);
}

TEST_F(UsbRcmLowLevelTest, UsbIoReadDeviceEcidCoversValidationBranches)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x66);

    EXPECT_EQ(usb_io::readDeviceEcid(nullptr, true), std::nullopt);

    deviceDescriptorResult = LIBUSB_ERROR_IO;
    EXPECT_EQ(usb_io::readDeviceEcid(handle, true), std::nullopt);

    deviceDescriptorResult = LIBUSB_SUCCESS;
    deviceDescriptor.iSerialNumber = 0;
    EXPECT_EQ(usb_io::readDeviceEcid(handle, true), std::nullopt);

    deviceDescriptor.iSerialNumber = 1;
    serialDescriptor = "0011";
    EXPECT_EQ(usb_io::readDeviceEcid(handle, true), std::nullopt);

    serialDescriptor =
        "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFG";
    EXPECT_EQ(usb_io::readDeviceEcid(handle, true), std::nullopt);

    serialDescriptor =
        "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
    auto ecid = usb_io::readDeviceEcid(handle, true);
    ASSERT_TRUE(ecid.has_value());
    EXPECT_EQ((*ecid)[0], 0x00);
    EXPECT_EQ((*ecid)[1], 0x11);
    EXPECT_EQ((*ecid)[31], 0xFF);
}

TEST_F(UsbRcmLowLevelTest,
       UsbIoAndProgressQueueCoverSilentFailureBranchesAndDescriptorErrors)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x67);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    EXPECT_EQ(usb_io::readRegister32(nullptr, 0x8000, false), std::nullopt);

    setTransferResult(0x00002008, LIBUSB_ERROR_IO);
    EXPECT_EQ(usb_io::readRegister32(handle, 0x2008, false), std::nullopt);

    EXPECT_EQ(usb_io::readDeviceEcid(nullptr, false), std::nullopt);

    deviceDescriptorResult = LIBUSB_ERROR_IO;
    EXPECT_EQ(usb_io::readDeviceEcid(handle, false), std::nullopt);

    deviceDescriptorResult = LIBUSB_SUCCESS;
    deviceDescriptor.iSerialNumber = 1;
    serialDescriptorResult = LIBUSB_ERROR_IO;
    EXPECT_EQ(usb_io::readDeviceEcid(handle, false), std::nullopt);

    serialDescriptorResult = serialResultSentinel;
    serialDescriptor = "0011";
    EXPECT_EQ(usb_io::readDeviceEcid(handle, false), std::nullopt);

    serialDescriptor =
        "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
    auto ecid = usb_io::readDeviceEcid(handle, false);
    ASSERT_TRUE(ecid.has_value());
    EXPECT_EQ((*ecid)[31], 0xFF);

    EXPECT_FALSE(
        progress_queue::readCpuPcqPointers(nullptr, false).has_value());

    controlTransferResults.clear();
    EXPECT_FALSE(progress_queue::readCpuPcqPointers(handle, false).has_value());

    controlTransferResults.clear();
    setRegister(0x2000, (1025u << 20));
    EXPECT_FALSE(progress_queue::readCpuPcqPointers(handle, false).has_value());

    EXPECT_FALSE(progress_queue::readCpuPcqEntries(nullptr, entries, false));

    controlTransferResults.clear();
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, false));

    controlTransferResults.clear();
    setRegister(0x2000, 0u);
    EXPECT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, false));
    EXPECT_TRUE(entries.empty());

    controlTransferResults.clear();
    setRegister(0x2000, (2u << 20) | (3u << 10));
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, false));

    controlTransferResults.clear();
    setRegister(0x2000, (1u << 20));
    setTransferResult(0x8000, LIBUSB_ERROR_IO);
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, false));
}

TEST_F(UsbRcmLowLevelTest, ProgressQueueReadsEntriesSortsAndFilters)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x77);

    std::vector<progress_queue::CPUProgressLogEntry> entries;
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(nullptr, entries, true));

    setRegister(0x2000, 0u);
    ASSERT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, true));
    EXPECT_TRUE(entries.empty());

    controlTransferResults.clear();
    setRegister(0x2000, (2u << 20) | (2u << 10));
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, true));

    controlTransferResults.clear();
    setRegister(0x2000, (3u << 20));
    setRegister(0x8000, 30u);
    setRegister(0x8004, 0xB0C10006u);
    setRegister(0x8008, 0u);
    setRegister(0x800C, 0x70C10007u);
    setRegister(0x8010, 10u);
    setRegister(0x8014, 0x70C0C002u);

    ASSERT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, false));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].timestamp, 10u);
    EXPECT_EQ(entries[0].progressCode, 0x70C0C002u);
    EXPECT_EQ(entries[1].timestamp, 30u);
    EXPECT_EQ(entries[1].progressCode, 0xB0C10006u);

    ASSERT_TRUE(
        progress_queue::readCpuPcqEntriesAfter(handle, 15u, entries, false));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().timestamp, 30u);
}

TEST_F(UsbRcmLowLevelTest, ProgressQueueInternalPointerValidationBranches)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x88);

    EXPECT_FALSE(progress_queue::readCpuPcqPointers(nullptr, true).has_value());

    controlTransferResults.clear();
    setRegister(0x2000, (1025u << 20));
    EXPECT_FALSE(progress_queue::readCpuPcqPointers(handle, true).has_value());
}

TEST_F(UsbRcmLowLevelTest,
       ProgressQueueCoversPointerReadFailureWraparoundAndZeroTimestamp)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x91);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    EXPECT_FALSE(progress_queue::readCpuPcqPointers(handle, true).has_value());

    setRegister(0x2000, (4u << 20) | (3u << 10) | 1u);
    setRegister(0x8018, 0u);
    setRegister(0x801C, 0x70C10007u);
    setRegister(0x8000, 11u);
    setRegister(0x8004, 0x70C0C002u);

    ASSERT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, true));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().timestamp, 11u);
    EXPECT_EQ(entries.front().progressCode, 0x70C0C002u);
}

TEST_F(UsbRcmLowLevelTest, ProgressQueueCoversProgressCodeReadFailure)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x92);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    setRegister(0x2000, (1u << 20));
    setRegister(0x8000, 12u);
    setTransferResult(0x8004, LIBUSB_ERROR_IO);

    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, true));
}

TEST_F(UsbRcmLowLevelTest, GetCpuBootSnapshotFindsBootProgressAndErrorCodes)
{
    std::vector<progress_queue::CPUProgressLogEntry> entries{
        {.timestamp = 5, .progressCode = 0x40, .queueId = 0},
        {.timestamp = 10, .progressCode = 0x70C0C002, .queueId = 0},
        {.timestamp = 20, .progressCode = 0x70C10007, .queueId = 0},
        {.timestamp = 30, .progressCode = 0xB0C10006, .queueId = 0},
    };

    auto snapshot = progress_queue::getCPUBootSnapshot(entries);
    ASSERT_TRUE(snapshot.latestBootModeSelDone.has_value());
    EXPECT_EQ(*snapshot.latestBootModeSelDone, 0x70C0C002u);
    ASSERT_TRUE(snapshot.latestProgressCode.has_value());
    EXPECT_EQ(*snapshot.latestProgressCode, 0x70C10007u);
    ASSERT_TRUE(snapshot.latestErrorCode.has_value());
    EXPECT_EQ(*snapshot.latestErrorCode, 0xB0C10006u);

    auto emptySnapshot = progress_queue::getCPUBootSnapshot(
        {{.timestamp = 1, .progressCode = 0x70C10007, .queueId = 0}});
    EXPECT_FALSE(emptySnapshot.latestBootModeSelDone.has_value());
    EXPECT_FALSE(emptySnapshot.latestProgressCode.has_value());
    EXPECT_FALSE(emptySnapshot.latestErrorCode.has_value());
}

TEST_F(UsbRcmLowLevelTest,
       ProgressQueueCoversInvalidIndicesFullQueueAndReadFailures)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x88);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    setRegister(0x2000, (2u << 20) | (3u << 10));
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, true));

    controlTransferResults.clear();
    setRegister(0x2000, (2u << 20));
    setRegister(0x8000, 5u);
    setRegister(0x8004, 0x70C0C002u);
    setRegister(0x8008, 8u);
    setRegister(0x800C, 0x70C10007u);
    ASSERT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, false));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries.front().timestamp, 5u);
    EXPECT_EQ(entries.back().timestamp, 8u);

    ASSERT_TRUE(
        progress_queue::readCpuPcqEntriesAfter(handle, 0u, entries, false));
    EXPECT_EQ(entries.size(), 2u);

    controlTransferResults.clear();
    setRegister(0x2000, (1u << 20));
    setTransferResult(0x8000, LIBUSB_ERROR_IO);
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, true));
}

TEST_F(UsbRcmLowLevelTest,
       ProgressQueueSkipsInvalidTimestampsAndFiltersAfterTimestamp)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x89);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    setRegister(0x2000, (3u << 20));
    setRegister(0x8000, 0xFFFFFFFFu);
    setRegister(0x8004, 0x70C10007u);
    setRegister(0x8008, 9u);
    setRegister(0x800C, 0x70C0C002u);
    setRegister(0x8010, 7u);
    setRegister(0x8014, 0xB0C10006u);

    ASSERT_TRUE(progress_queue::readCpuPcqEntries(handle, entries, true));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].timestamp, 7u);
    EXPECT_EQ(entries[1].timestamp, 9u);

    ASSERT_TRUE(
        progress_queue::readCpuPcqEntriesAfter(handle, 7u, entries, false));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().timestamp, 9u);

    ASSERT_TRUE(
        progress_queue::readCpuPcqEntriesAfter(handle, 100u, entries, false));
    EXPECT_TRUE(entries.empty());
}

TEST_F(UsbRcmLowLevelTest, ProgressQueueAfterReadPropagatesUnderlyingFailure)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x90);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    setTransferResult(0x00002000, LIBUSB_ERROR_IO);
    EXPECT_FALSE(
        progress_queue::readCpuPcqEntriesAfter(handle, 1u, entries, true));
}

TEST_F(UsbRcmLowLevelTest,
       CpuBootSnapshotHandlesBootSelectionOnlyAndNoBootSelection)
{
    const std::vector<progress_queue::CPUProgressLogEntry> bootOnlyEntries{
        {.timestamp = 5, .progressCode = 0x70C0C002u, .queueId = 0},
    };
    auto bootOnlySnapshot = progress_queue::getCPUBootSnapshot(bootOnlyEntries);
    ASSERT_TRUE(bootOnlySnapshot.latestBootModeSelDone.has_value());
    ASSERT_TRUE(bootOnlySnapshot.latestProgressCode.has_value());
    EXPECT_EQ(*bootOnlySnapshot.latestProgressCode, 0x70C0C002u);
    EXPECT_FALSE(bootOnlySnapshot.latestErrorCode.has_value());

    const std::vector<progress_queue::CPUProgressLogEntry> noBootSelEntries{
        {.timestamp = 8, .progressCode = 0x70C10007u, .queueId = 0},
        {.timestamp = 9, .progressCode = 0xB0C10006u, .queueId = 0},
    };
    auto emptySnapshot = progress_queue::getCPUBootSnapshot(noBootSelEntries);
    EXPECT_FALSE(emptySnapshot.latestBootModeSelDone.has_value());
    EXPECT_FALSE(emptySnapshot.latestProgressCode.has_value());
    EXPECT_FALSE(emptySnapshot.latestErrorCode.has_value());
}

TEST_F(UsbRcmLowLevelTest, ProgressQueueCoversInvalidEndIndexValidationBranch)
{
    auto handle = reinterpret_cast<libusb_device_handle*>(0x93);
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    controlTransferResults.clear();
    setRegister(0x2000, (2u << 20) | 2u);
    EXPECT_FALSE(progress_queue::readCpuPcqEntries(handle, entries, true));
}

TEST_F(UsbRcmLowLevelTest,
       CpuBootSnapshotSkipsOtherRomProgressBeforeBootSelection)
{
    const std::vector<progress_queue::CPUProgressLogEntry> entries{
        {.timestamp = 1, .progressCode = 0x70C0C002u, .queueId = 0},
        {.timestamp = 2, .progressCode = 0x70C0C001u, .queueId = 0},
    };

    auto snapshot = progress_queue::getCPUBootSnapshot(entries);
    ASSERT_TRUE(snapshot.latestBootModeSelDone.has_value());
    EXPECT_EQ(*snapshot.latestBootModeSelDone, 0x70C0C002u);
    ASSERT_TRUE(snapshot.latestProgressCode.has_value());
    EXPECT_EQ(*snapshot.latestProgressCode, 0x70C0C001u);
    EXPECT_FALSE(snapshot.latestErrorCode.has_value());
}
