#include "../recovery_tool/usbrcm_recovery_tool/get_recovery_status.hpp"
#include "../recovery_tool/usbrcm_recovery_tool/progress_code_queue.hpp"
#include "../recovery_tool/usbrcm_recovery_tool/usb_device_manager.hpp"
#include "../recovery_tool/usbrcm_recovery_tool/usb_io.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace
{

struct FakeStatusDevice
{
    std::string portPath;
    bool sessionValid = true;
    bool rcmMode = true;
    bool pcqSuccess = true;
    std::vector<progress_queue::CPUProgressLogEntry> entries;
};

bool usbContextValid = true;
std::vector<libusb_device*> discoveredDevices;
std::map<libusb_device*, FakeStatusDevice> fakeStatusDevices;
std::map<libusb_device_handle*, libusb_device*> handleToDevice;
progress_queue::CPUBootSnapshot snapshotToReturn{};
std::optional<std::array<uint8_t, EcidParser::ECID_SIZE>> ecidToReturn;

libusb_device* makeStatusDevice(int id, FakeStatusDevice device)
{
    auto* ptr = reinterpret_cast<libusb_device*>(
        static_cast<uintptr_t>(0x3000 + id * 0x100));
    fakeStatusDevices.emplace(ptr, std::move(device));
    return ptr;
}

class GetRecoveryStatusTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        usbContextValid = true;
        discoveredDevices.clear();
        fakeStatusDevices.clear();
        handleToDevice.clear();
        snapshotToReturn = {};
        ecidToReturn = std::nullopt;
    }
};

} // namespace

namespace usb
{

UsbContext::UsbContext() noexcept
{
    context =
        usbContextValid ? reinterpret_cast<libusb_context*>(0x1) : nullptr;
}

UsbContext::~UsbContext() noexcept = default;

UsbContext& UsbContext::operator=(UsbContext&& other) noexcept
{
    if (this != &other)
    {
        context = other.context;
        other.context = nullptr;
    }
    return *this;
}

UsbDevice::UsbDevice(libusb_device* dev, std::string path) noexcept :
    device(dev), portPath(std::move(path))
{}

UsbDevice::~UsbDevice() noexcept = default;

UsbDevice::UsbDevice(UsbDevice&& other) noexcept :
    device(other.device), portPath(std::move(other.portPath))
{
    other.device = nullptr;
    other.portPath.clear();
}

UsbDevice& UsbDevice::operator=(UsbDevice&& other) noexcept
{
    if (this != &other)
    {
        device = other.device;
        portPath = std::move(other.portPath);
        other.device = nullptr;
        other.portPath.clear();
    }
    return *this;
}

UsbControlSession::UsbControlSession(libusb_device* device,
                                     const UsbContext& ctx) noexcept
{
    if (device == nullptr || !ctx.isValid())
    {
        return;
    }

    auto& fake = fakeStatusDevices.at(device);
    portPath = fake.portPath;
    if (!fake.sessionValid)
    {
        return;
    }

    handle = reinterpret_cast<libusb_device_handle*>(
        static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(device) + 0x55));
    handleToDevice[handle] = device;
}

UsbControlSession::~UsbControlSession() noexcept = default;

UsbControlSession&
    UsbControlSession::operator=(UsbControlSession&& other) noexcept
{
    if (this != &other)
    {
        handle = other.handle;
        portPath = std::move(other.portPath);
        other.handle = nullptr;
        other.portPath.clear();
    }
    return *this;
}

std::vector<UsbDevice> findDevicesByVidPid(const UsbContext&, bool) noexcept
{
    std::vector<UsbDevice> result;
    for (auto* device : discoveredDevices)
    {
        result.emplace_back(device, fakeStatusDevices.at(device).portPath);
    }
    return result;
}

bool isDeviceInRcmMode(libusb_device* device, const bool) noexcept
{
    return fakeStatusDevices.at(device).rcmMode;
}

} // namespace usb

namespace progress_queue
{

bool readCpuPcqEntries(libusb_device_handle* handle,
                       std::vector<CPUProgressLogEntry>& entries, const bool)
{
    auto device = handleToDevice.at(handle);
    const auto& fake = fakeStatusDevices.at(device);
    entries = fake.entries;
    return fake.pcqSuccess;
}

CPUBootSnapshot getCPUBootSnapshot(const std::vector<CPUProgressLogEntry>&)
{
    return snapshotToReturn;
}

} // namespace progress_queue

namespace usb_io
{

std::optional<std::array<uint8_t, EcidParser::ECID_SIZE>>
    readDeviceEcid(libusb_device_handle*, bool) noexcept
{
    return ecidToReturn;
}

} // namespace usb_io

#include "../recovery_tool/usbrcm_recovery_tool/get_recovery_status.cpp"

TEST_F(GetRecoveryStatusTest, UsbInitializationFailureReturnsCriticalError)
{
    usbContextValid = false;

    nlohmann::json output;
    EXPECT_FALSE(getRecoveryStatus(output, true));
    EXPECT_EQ(output["Error"], "Failed to initialize USB subsystem");
}

TEST_F(GetRecoveryStatusTest, NoDevicesReturnsNonCriticalErrorObject)
{
    nlohmann::json output;
    EXPECT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output["Error"],
              "No USB devices found with Vera VID:PID=0x0955:0x7410");
}

TEST_F(GetRecoveryStatusTest, SuccessfulRecoveryCompletePopulatesDeviceEntry)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "1-4",
                            .entries = {{.timestamp = 10,
                                         .progressCode = 0x70C0C002,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C002;
    snapshotToReturn.latestProgressCode =
        ProgressCodeParser::RecoveryCompleteCodes::CPU0;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::array<uint8_t, EcidParser::ECID_SIZE>{};

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    ASSERT_TRUE(output.is_array());
    ASSERT_EQ(output.size(), 1u);

    const auto& entry = output[0];
    EXPECT_EQ(entry["USB Port Path"], "1-4");
    EXPECT_EQ(entry["Recovery Status"], "Recovery Complete");
    EXPECT_EQ(entry["DOT Blob Required"], "No");
    EXPECT_EQ(entry["CPU Instance"], 0);
    EXPECT_TRUE(entry["Boot Selection Info"].get<std::string>().find(
                    "Boot mode") != std::string::npos);
    EXPECT_TRUE(entry["Last Progress Code"].get<std::string>().find(
                    "PSC_RT_PC_PLDM_T5_READY") != std::string::npos);
}

TEST_F(GetRecoveryStatusTest,
       RecoveryBootWithoutRcmEndpointsSetsConsistencyError)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "2-1",
                            .rcmMode = false,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C002,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C002;
    snapshotToReturn.latestProgressCode = 0x70C10007;
    ecidToReturn = std::nullopt;

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, true));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "Unknown");
    EXPECT_EQ(entry["DOT Blob Required"], "Unknown");
    EXPECT_EQ(entry["Error"],
              "Boot mode indicates recovery but device lacks RCM endpoints");
}

TEST_F(GetRecoveryStatusTest,
       NonRecoveryBootWithRcmModeAlsoSetsConsistencyError)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "2-2",
                            .rcmMode = true,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C042,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C042;
    snapshotToReturn.latestProgressCode = 0x70C10007;

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "Unknown");
    EXPECT_EQ(entry["Error"],
              "Device has RCM endpoints but boot mode is not recovery");
}

TEST_F(GetRecoveryStatusTest, DeviceOpenAndProgressFailuresAreReportedPerDevice)
{
    auto* badOpen = makeStatusDevice(1, FakeStatusDevice{.portPath = "3-1",
                                                         .sessionValid = false,
                                                         .entries = {}});
    auto* badPcq = makeStatusDevice(2, FakeStatusDevice{.portPath = "3-2",
                                                        .pcqSuccess = false,
                                                        .entries = {}});
    auto* emptyPcq =
        makeStatusDevice(3, FakeStatusDevice{.portPath = "3-3", .entries = {}});
    discoveredDevices = {badOpen, badPcq, emptyPcq};

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    ASSERT_EQ(output.size(), 3u);
    EXPECT_EQ(output[0]["Error"], "Failed to open USB device");
    EXPECT_EQ(output[1]["Error"], "Failed to read progress codes from device");
    EXPECT_EQ(output[2]["Error"], "No progress code entries found for device");
}

TEST_F(GetRecoveryStatusTest,
       UnknownProgressCodesUseHexFallbackAndIncludeLastErrorCode)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "4-1",
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70000001,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C002;
    snapshotToReturn.latestProgressCode = 0x70000001;
    snapshotToReturn.latestErrorCode = 0xB0FFFFFF;
    ecidToReturn = std::array<uint8_t, EcidParser::ECID_SIZE>{};

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "In Recovery");
    EXPECT_EQ(entry["CPU Instance"], 0);
    EXPECT_EQ(entry["Last Progress Code"], "0x70000001");
    EXPECT_EQ(entry["Last Error Code"], "0xB0FFFFFF");
}

TEST_F(GetRecoveryStatusTest,
       MissingBootSelectionLeavesRecoveryUnknownButUsesCpuFromProgress)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "5-1",
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x71C2C002,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = std::nullopt;
    snapshotToReturn.latestProgressCode =
        ProgressCodeParser::RecoveryCompleteCodes::CPU1;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::nullopt;

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "Unknown");
    EXPECT_EQ(entry["CPU Instance"], 1);
    EXPECT_EQ(entry["Boot Selection Info"], "");
    EXPECT_EQ(entry["DOT Blob Required"], "Unknown");
}

TEST_F(GetRecoveryStatusTest,
       BootSelectionWithoutMetadataKeepsStatusUnknownAndCanRequireDotBlob)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "6-1",
                            .rcmMode = false,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C10007,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C10007;
    snapshotToReturn.latestProgressCode = std::nullopt;
    snapshotToReturn.latestErrorCode = std::nullopt;

    std::array<uint8_t, EcidParser::ECID_SIZE> ecid{};
    ecid[EcidParser::BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10;
    ecidToReturn = ecid;

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "Unknown");
    EXPECT_EQ(entry["CPU Instance"], "Unknown");
    EXPECT_EQ(entry["Last Progress Code"], "");
    EXPECT_EQ(entry["DOT Blob Required"], "Yes");
    EXPECT_EQ(entry["Error"], "");
}

TEST_F(GetRecoveryStatusTest,
       NonRecoveryBootWithoutRcmEndpointsKeepsStatusWithoutConsistencyError)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "6-2",
                            .rcmMode = false,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C042,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C042;
    snapshotToReturn.latestProgressCode = 0x70C10007;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::nullopt;

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, true));
    const auto& entry = output[0];
    EXPECT_EQ(entry["Recovery Status"], "Not in Recovery");
    EXPECT_EQ(entry["Error"], "");
    EXPECT_EQ(entry["DOT Blob Required"], "Unknown");
}

TEST_F(GetRecoveryStatusTest, SilentErrorPathsCoverAdditionalBranches)
{
    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    EXPECT_EQ(output["Error"],
              "No USB devices found with Vera VID:PID=0x0955:0x7410");

    auto* badOpen = makeStatusDevice(1, FakeStatusDevice{.portPath = "7-1",
                                                         .sessionValid = false,
                                                         .entries = {}});
    discoveredDevices = {badOpen};
    ASSERT_TRUE(getRecoveryStatus(output, false));
    EXPECT_EQ(output[0]["Error"], "Failed to open USB device");

    auto* badPcq = makeStatusDevice(2, FakeStatusDevice{.portPath = "7-2",
                                                        .pcqSuccess = false,
                                                        .entries = {}});
    discoveredDevices = {badPcq};
    ASSERT_TRUE(getRecoveryStatus(output, false));
    EXPECT_EQ(output[0]["Error"], "Failed to read progress codes from device");

    auto* emptyPcq =
        makeStatusDevice(3, FakeStatusDevice{.portPath = "7-3", .entries = {}});
    discoveredDevices = {emptyPcq};
    ASSERT_TRUE(getRecoveryStatus(output, false));
    EXPECT_EQ(output[0]["Error"], "No progress code entries found for device");

    auto* recoveryNoRcm = makeStatusDevice(
        4, FakeStatusDevice{.portPath = "7-4",
                            .rcmMode = false,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C002,
                                         .queueId = 0}}});
    discoveredDevices = {recoveryNoRcm};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C002;
    snapshotToReturn.latestProgressCode = 0x70C10007;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::nullopt;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    EXPECT_EQ(output[0]["Error"],
              "Boot mode indicates recovery but device lacks RCM endpoints");
}

TEST_F(GetRecoveryStatusTest, VerboseFailurePathsCoverDiagnosticBranches)
{
    nlohmann::json output;

    auto* badOpen = makeStatusDevice(1, FakeStatusDevice{.portPath = "8-1",
                                                         .sessionValid = false,
                                                         .entries = {}});
    discoveredDevices = {badOpen};
    ASSERT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output[0]["Error"], "Failed to open USB device");

    auto* badPcq = makeStatusDevice(2, FakeStatusDevice{.portPath = "8-2",
                                                        .pcqSuccess = false,
                                                        .entries = {}});
    discoveredDevices = {badPcq};
    ASSERT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output[0]["Error"], "Failed to read progress codes from device");

    auto* emptyPcq =
        makeStatusDevice(3, FakeStatusDevice{.portPath = "8-3", .entries = {}});
    discoveredDevices = {emptyPcq};
    ASSERT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output[0]["Error"], "No progress code entries found for device");

    auto* ecidMissing = makeStatusDevice(
        4, FakeStatusDevice{.portPath = "8-4",
                            .rcmMode = false,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C042,
                                         .queueId = 0}}});
    discoveredDevices = {ecidMissing};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C042;
    snapshotToReturn.latestProgressCode = 0x70C10007;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::nullopt;

    ASSERT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output[0]["Error"], "");
    EXPECT_EQ(output[0]["DOT Blob Required"], "Unknown");
}

TEST_F(GetRecoveryStatusTest,
       RecoveryBootWithoutLatestProgressUsesBootSelectionCpuInstance)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "8-5",
                            .rcmMode = true,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C002,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C002;
    snapshotToReturn.latestProgressCode = std::nullopt;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::array<uint8_t, EcidParser::ECID_SIZE>{};

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, false));
    const auto& entry = output[0];
    EXPECT_EQ(entry["CPU Instance"], 0);
    EXPECT_EQ(entry["Recovery Status"], "In Recovery");
    EXPECT_EQ(entry["Last Progress Code"], "");
    EXPECT_EQ(entry["DOT Blob Required"], "No");
    EXPECT_TRUE(entry["Boot Selection Info"].get<std::string>().find(
                    "Boot mode") != std::string::npos);
}

TEST_F(GetRecoveryStatusTest, VerboseRcmConsistencyWarningCoversRemainingBranch)
{
    auto* device = makeStatusDevice(
        1, FakeStatusDevice{.portPath = "8-6",
                            .rcmMode = true,
                            .entries = {{.timestamp = 1,
                                         .progressCode = 0x70C0C042,
                                         .queueId = 0}}});
    discoveredDevices = {device};
    snapshotToReturn.latestBootModeSelDone = 0x70C0C042;
    snapshotToReturn.latestProgressCode = 0x70C10007;
    snapshotToReturn.latestErrorCode = std::nullopt;
    ecidToReturn = std::array<uint8_t, EcidParser::ECID_SIZE>{};

    nlohmann::json output;
    ASSERT_TRUE(getRecoveryStatus(output, true));
    EXPECT_EQ(output[0]["Recovery Status"], "Unknown");
    EXPECT_EQ(output[0]["Error"],
              "Device has RCM endpoints but boot mode is not recovery");
    EXPECT_EQ(output[0]["DOT Blob Required"], "No");
}
