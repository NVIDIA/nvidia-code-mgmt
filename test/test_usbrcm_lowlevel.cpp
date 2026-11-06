#include <libusb-1.0/libusb.h>
#include <sys/wait.h>

#include <gpiod.hpp>
#include <sdbusplus/bus.hpp>

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

class UsbRcmLowLevelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        systemExpectations.clear();
        systemCommands.clear();
        controlTransferResults.clear();
        gpiod::gpioFakes().clear();
        test::usbrcm_fake_bus::reset();
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

// --- GPIO fake helpers ---

static void gpioFindFail(const std::string& name)
{
    gpiod::gpioFakes()[name].findSucceeds = false;
}

static void gpioRequestThrow(const std::string& name,
                             const std::string& msg = "mock gpio error")
{
    auto& s = gpiod::gpioFakes()[name];
    s.findSucceeds = true;
    s.requestThrows = true;
    s.throwMsg = msg;
}

static void gpioSetValueThrow(const std::string& name,
                              const std::string& msg = "mock set_value error")
{
    auto& s = gpiod::gpioFakes()[name];
    s.findSucceeds = true;
    s.setValueThrows = true;
    s.throwMsg = msg;
}

static RecoveryPinConfig makeC2Config()
{
    RecoveryPinConfig cfg;
    cfg.resetPins = {"B0_M1_IST_SYS_RST_L-O"};
    cfg.strapPins = {
        "B0_M1_CPU_FORCED_RECOVERY_L-O", "B0_M1_CPU_BOOT_DEV_SEL0-O",
        "B0_M1_CPU_BOOT_DEV_SEL1-O",     "B0_M1_CPU_BOOT_DEV_SEL2-O",
        "B0_M1_CPU_RECOVERY_TYPE0-O",    "B0_M1_CPU_RECOVERY_TYPE1-O",
    };
    cfg.strapActiveValues = {0, 0, 0, 0, 0, 1};
    cfg.strapDefaultValues = {1, 0, 0, 0, 0, 1};
    return cfg;
}

// --- forceRecoveryMode tests ---

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHandlesResetGpioNotFound)
{
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioFindFail("B0_M1_IST_SYS_RST_L-O");
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "No reset GPIO found");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHandlesResetGpioClaimFailure)
{
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioRequestThrow("B0_M1_IST_SYS_RST_L-O", "gpio busy");
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "gpio busy");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHandlesResetReleaseFailure)
{
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioSetValueThrow("B0_M1_IST_SYS_RST_L-O", "set_value failed");
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "Reset release failed: set_value failed");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoverySuccessfulPath)
{
    nlohmann::json out;
    auto cfg = makeC2Config();
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Successful");
    EXPECT_EQ(gpiod::gpioFakes()["B0_M1_IST_SYS_RST_L-O"].lastSetValue, 1);
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryDbusResetFailsGracefully)
{
    // Empty resetPins → D-Bus mode: assertReset() calls dbusReset(true, cfg).
    // Inject a fake D-Bus error so forceRecoveryMode catches the exception
    // and returns a Failed status, exercising the assertReset D-Bus branch.
    test::usbrcm_fake_bus::setCallThrows(true, "mock D-Bus error");
    RecoveryPinConfig cfg;
    cfg.dbusService = "com.example.Service";
    cfg.dbusObject = "/com/example/obj";
    cfg.dbusInterface = "com.example.Iface";
    cfg.dbusMethod = "Reset";
    cfg.strapPins = {"STRAP_A"};
    cfg.strapActiveValues = {0};
    cfg.strapDefaultValues = {1};

    nlohmann::json out;
    EXPECT_NO_THROW(forceRecoveryMode(cfg, out));
    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_TRUE(out.contains("Error"));
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryDbusResetSuccessPath)
{
    // Empty resetPins → D-Bus mode: assertReset calls dbusReset(true, cfg)
    // via the fake bus (no exception). Phase 1 returns an empty resetLines
    // vector, so Phase 3 releaseReset takes the D-Bus else-branch and calls
    // dbusReset(false, cfg) — covering both D-Bus branches in force_recovery.
    RecoveryPinConfig cfg;
    cfg.dbusService = "com.example.Service";
    cfg.dbusObject = "/com/example/obj";
    cfg.dbusInterface = "com.example.Iface";
    cfg.dbusMethod = "Reset";

    nlohmann::json out;
    EXPECT_NO_THROW(forceRecoveryMode(cfg, out));
    EXPECT_EQ(out["Status"], "Successful");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryHandlesStrapGpioClaimThrow)
{
    // gpioRequestThrow covers claimGpio's catch block (request throws) rather
    // than the find-fail path, exercising the false-branch of if(!line) and
    // the catch body inside claimGpio.
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioRequestThrow("B0_M1_CPU_FORCED_RECOVERY_L-O", "gpio busy");
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
}

TEST_F(UsbRcmLowLevelTest, StrapFailureRollsBackAlreadyDrivenStraps)
{
    // Fail on the *second* strap so strapLines is non-empty when the failure
    // path runs. Strap 0 was driven to its active value (0) and must be rolled
    // back to its default (1) rather than left in a half-strapped state, and
    // the reset must be de-asserted since nothing retains it past return.
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioRequestThrow("B0_M1_CPU_BOOT_DEV_SEL0-O");
    forceRecoveryMode(cfg, out);

    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "Strap GPIO failed: B0_M1_CPU_BOOT_DEV_SEL0-O");
    EXPECT_EQ(gpiod::gpioFakes()["B0_M1_CPU_FORCED_RECOVERY_L-O"].lastSetValue,
              cfg.strapDefaultValues[0]);
    EXPECT_EQ(gpiod::gpioFakes()["B0_M1_IST_SYS_RST_L-O"].lastSetValue, 1);
}

TEST_F(UsbRcmLowLevelTest, StrapFailureRollbackToleratesSetValueThrow)
{
    // A strap that cannot be rolled back must not prevent the remaining
    // rollback work or the reset release — covers the catch inside the
    // rollback loop.
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioSetValueThrow("B0_M1_CPU_FORCED_RECOVERY_L-O", "rollback failed");
    gpioRequestThrow("B0_M1_CPU_BOOT_DEV_SEL0-O");
    EXPECT_NO_THROW(forceRecoveryMode(cfg, out));

    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "Strap GPIO failed: B0_M1_CPU_BOOT_DEV_SEL0-O");
    EXPECT_EQ(gpiod::gpioFakes()["B0_M1_IST_SYS_RST_L-O"].lastSetValue, 1);
}

TEST_F(UsbRcmLowLevelTest, StrapFailureToleratesResetReleaseThrow)
{
    // Reset release failing during the strap-failure path must be logged and
    // swallowed so the original strap error is what gets reported — covers the
    // catch around releaseReset in the failure path.
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioRequestThrow("B0_M1_CPU_BOOT_DEV_SEL0-O");
    gpioSetValueThrow("B0_M1_IST_SYS_RST_L-O", "reset release failed");
    EXPECT_NO_THROW(forceRecoveryMode(cfg, out));

    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"], "Strap GPIO failed: B0_M1_CPU_BOOT_DEV_SEL0-O");
}

TEST_F(UsbRcmLowLevelTest, ForceRecoveryTwoResetPinsCoversLoopBackEdge)
{
    // Two reset pins make assertReset and releaseReset iterate twice,
    // covering the for-loop back-edge branches in both functions.
    nlohmann::json out;
    auto cfg = makeC2Config();
    cfg.resetPins.push_back("B0_M2_IST_SYS_RST_L-O");
    forceRecoveryMode(cfg, out);
    EXPECT_EQ(out["Status"], "Successful");
}

// --- setGPIODefaultPinStates tests ---

TEST_F(UsbRcmLowLevelTest, SetGPIODefaultPinStatesHandlesPartialAndFullSuccess)
{
    nlohmann::json out;
    auto cfg = makeC2Config();

    gpioRequestThrow("B0_M1_CPU_FORCED_RECOVERY_L-O");
    setGPIODefaultPinStates(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
    EXPECT_EQ(out["Error"],
              "GPIO restore failed: B0_M1_CPU_FORCED_RECOVERY_L-O");

    out.clear();
    gpiod::gpioFakes().clear();
    setGPIODefaultPinStates(cfg, out);
    EXPECT_EQ(out["Status"], "Successful");
}

TEST_F(UsbRcmLowLevelTest,
       SetGPIODefaultPinStatesMultipleFailuresCoversSeparator)
{
    // Two failing pins exercise the ", " separator branch in the failure
    // string build loop (the first pin uses "" prefix, the second uses ", ").
    nlohmann::json out;
    auto cfg = makeC2Config();
    gpioRequestThrow("B0_M1_CPU_FORCED_RECOVERY_L-O");
    gpioRequestThrow("B0_M1_CPU_BOOT_DEV_SEL0-O");
    setGPIODefaultPinStates(cfg, out);
    EXPECT_EQ(out["Status"], "Failed");
    auto err = out["Error"].get<std::string>();
    EXPECT_NE(err.find(", "), std::string::npos);
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
