// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

/**
 * Unit tests for UsbDfuRecovery and loadBundleManifest.
 *
 * Pattern follows test_mcu_recovery.cpp:
 *   - usbdfu_fakes/gpiod.hpp shadows the real <gpiod.hpp> via include path
 *   - #define private public exposes internals for white-box testing
 *   - Linker wraps intercept fork/waitpid (executeProcess) and nanosleep
 *     (std::this_thread::sleep_for) so tests run instantly without hardware
 *   - USB DFU enumeration is checked against a fake sysfs tree in a temp dir
 *   - Package layout tests build a fake <UUID>/<component id>/<file> tree
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// Must come before the production header so that the fake gpiod is picked up
// (usbdfu_fakes/ is first in the compiler include path for this target).
// The fake is header-only and provides gpiod::find_line / gpiod::line.
#include "gpiod.hpp" // fake, resolved via usbdfu_fakes/

// Pull in the shared headers before gtest (the generated phosphor-logging
// error types have a member named FAIL that collides with gtest's FAIL()
// macro) and before redefining `private` so that only the class under test
// is affected.
#include "message_registry.hpp"
#include "usb_sysfs.hpp"

#include "gtest/gtest.h"

#define private public
#include "perform_dfu_recovery.hpp"
#undef private

// ============================================================================
// Linker-wrap state
// ============================================================================

namespace
{

/// Value returned by __wrap_fork.
///   < 0  -> fork fails (parent gets error)
///   > 0  -> fake child pid (parent path runs; child branch skipped)
///   = 0  -> child path -- NEVER set in tests (would run execvp in test
///   process)
pid_t g_forkRet = 1;

/// Status word written through the waitpid `status` pointer.
///   0         -> WIFEXITED=true, WEXITSTATUS=0  (success)
///   (1 << 8)  -> WIFEXITED=true, WEXITSTATUS=1  (failure)
int g_waitpidStatus = 0;

/// Value returned by __wrap_waitpid.
///   Use a valid pid (>0) for normal completion; -1 to simulate waitpid error.
pid_t g_waitpidRet = 1;

/// Number of __wrap_waitpid calls so far (i.e. dfu-util invocations).
std::size_t g_waitpidCallCount = 0;

/// When > 0, the Nth waitpid call reports exit status 1 regardless of
/// g_waitpidStatus -- lets a test fail exactly one dfu-util invocation.
std::size_t g_waitpidFailOnCall = 0;

/// Hook run on every intercepted sleep (lets a test change state "while"
/// production code waits, e.g. make the USB device appear on a later poll).
std::function<void()> g_onSleep;

} // namespace

extern "C" pid_t __wrap_fork()
{
    return g_forkRet;
}

extern "C" pid_t __wrap_waitpid(pid_t /*pid*/, int* status, int /*options*/)
{
    ++g_waitpidCallCount;
    if (status)
    {
        *status = (g_waitpidFailOnCall > 0 &&
                   g_waitpidCallCount == g_waitpidFailOnCall)
                      ? (1 << 8)
                      : g_waitpidStatus;
    }
    return g_waitpidRet;
}

// Make std::this_thread::sleep_for a no-op so tests don't actually sleep.
// Both nanosleep and clock_nanosleep are intercepted because which one glibc
// routes sleep_for through is implementation-defined.
extern "C" int __wrap_nanosleep(const struct timespec* /*req*/,
                                struct timespec* /*rem*/)
{
    if (g_onSleep)
        g_onSleep();
    return 0;
}

extern "C" int __wrap_clock_nanosleep(clockid_t /*clock*/, int /*flags*/,
                                      const struct timespec* /*req*/,
                                      struct timespec* /*rem*/)
{
    if (g_onSleep)
        g_onSleep();
    return 0;
}

// ============================================================================
// Test helpers
// ============================================================================

namespace
{

/// GPIO names used across all tests
constexpr const char* kRecovery = "RECOVERY_GPIO";
constexpr const char* kReset = "RESET_GPIO";
constexpr const char* kSpiMux = "SPIMUX_GPIO";

/// Fake sysfs identity of the BootROM DFU device
constexpr const char* kVid = "2245";
constexpr const char* kPid = "2700";

/// Error codes as the tool reports them in out["ErrorCode"]
constexpr uint8_t ec(USBDFURecoveryErrorCode c)
{
    return static_cast<uint8_t>(c);
}
constexpr uint8_t kGpioAssertFailed =
    ec(USBDFURecoveryErrorCode::GPIOAssertFailed);
constexpr uint8_t kBundleSendFailed =
    ec(USBDFURecoveryErrorCode::BundleSendFailed);
constexpr uint8_t kFirmwareFlashFailed =
    ec(USBDFURecoveryErrorCode::FirmwareFlashFailed);
constexpr uint8_t kGpioDeassertFailed =
    ec(USBDFURecoveryErrorCode::GPIODeassertFailed);
constexpr uint8_t kDfuEnumerationFailed =
    ec(USBDFURecoveryErrorCode::DfuEnumerationFailed);
constexpr uint8_t kInvalidConfiguration =
    ec(USBDFURecoveryErrorCode::InvalidConfiguration);
constexpr uint8_t kPackageIncomplete =
    ec(USBDFURecoveryErrorCode::PackageIncomplete);
constexpr uint8_t kRecoveryUBootDfuTimeout =
    ec(USBDFURecoveryErrorCode::RecoveryUBootDfuTimeout);

/// Register all three standard GPIO lines as present in the fake.
void registerAllLines()
{
    test::usbdfu_fake_gpio::lines[kRecovery] = {};
    test::usbdfu_fake_gpio::lines[kReset] = {};
    test::usbdfu_fake_gpio::lines[kSpiMux] = {};
}

/// Create a dummy file of `size` bytes at the given path (parents must exist).
void writeBytes(const std::filesystem::path& p, std::size_t size)
{
    std::ofstream f(p, std::ios::trunc | std::ios::binary);
    f.exceptions(std::ios::failbit | std::ios::badbit);
    std::string data(size, '\xA5');
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/// Create an empty dummy file at the given path (parents must exist).
void touchFile(const std::filesystem::path& p)
{
    writeBytes(p, 1);
}

/// Write text content to a file.
void writeFile(const std::filesystem::path& p, std::string_view content)
{
    std::ofstream f(p, std::ios::trunc);
    f.exceptions(std::ios::failbit | std::ios::badbit);
    f << content;
}

// ============================================================================
// Fixture
// ============================================================================

class UsbDfuRecoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        test::usbdfu_fake_gpio::reset();
        g_forkRet = 1;
        g_waitpidStatus = 0;
        g_waitpidRet = 1;
        g_waitpidCallCount = 0;
        g_waitpidFailOnCall = 0;
        g_onSleep = nullptr;

        tmpDir_ = std::filesystem::temp_directory_path() /
                  ("usbdfu_recovery_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(tmpDir_);
        std::filesystem::create_directories(tmpDir_);
        sysfsRoot_ = tmpDir_ / "sys";
        std::filesystem::create_directories(sysfsRoot_);
    }

    void TearDown() override
    {
        g_onSleep = nullptr;
        std::filesystem::remove_all(tmpDir_);
    }

    /// Build a Config with all delays set to 0, a dummy dfu-util path, the
    /// fake sysfs root and no image truncation (tests opt in explicitly).
    UsbDfuRecovery::Config makeConfig() const
    {
        UsbDfuRecovery::Config cfg;
        cfg.recoveryGpioName = kRecovery;
        cfg.resetGpioName = kReset;
        cfg.spiMuxGpioName = kSpiMux;
        cfg.dfuUtilPath = "/fake/dfu-util";
        cfg.usbSysfsRoot = sysfsRoot_.string();
        cfg.flashLengthBytes = 0;
        cfg.bundleStepDelaySecs = 0;
        // Do not wait for recovery U-Boot's DFU targets unless a test asks
        cfg.uBootDfuTimeoutSecs = 0;
        cfg.uBootDfuPollSecs = 1;
        cfg.postFlashSettleSecs = 0;
        // 1 s / 500 ms -> polls at 0, 0.5 and 1 s (three attempts)
        cfg.dfuEnumerationTimeoutSecs = 1;
        cfg.dfuEnumerationPollMs = 500;
        return cfg;
    }

    /// Add a USB device entry to the fake sysfs tree.
    void fakeUsbDevice(const std::string& name, const std::string& vid,
                       const std::string& pid) const
    {
        auto dev = sysfsRoot_ / name;
        std::filesystem::create_directories(dev);
        writeFile(dev / "idVendor", vid + "\n");
        writeFile(dev / "idProduct", pid + "\n");
    }

    /// GPIO lines present and the BootROM DFU device already enumerated.
    void makeRecoveryReady() const
    {
        registerAllLines();
        fakeUsbDevice("1-1", kVid, kPid);
    }

    std::filesystem::path tmpDir_;
    std::filesystem::path sysfsRoot_;
};

} // namespace

// ============================================================================
// requestOutputLine tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, RequestOutputLine_LineNotFound)
{
    // No line registered -> find_line returns invalid -> errorMsg set
    std::string errorMsg;
    auto line = UsbDfuRecovery::requestOutputLine("MISSING_GPIO", 1, errorMsg);

    EXPECT_FALSE(static_cast<bool>(line));
    EXPECT_NE(errorMsg.find("MISSING_GPIO"), std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, RequestOutputLine_FindLineThrows)
{
    // libgpiod's find_line() opens every chip and can throw; the noexcept
    // helper must report that instead of terminating the process.
    test::usbdfu_fake_gpio::throwOnFindLine = true;

    std::string errorMsg;
    auto line = UsbDfuRecovery::requestOutputLine(kRecovery, 1, errorMsg);

    EXPECT_FALSE(static_cast<bool>(line));
    EXPECT_NE(errorMsg.find("GPIO lookup failed"), std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, RequestOutputLine_RequestThrows)
{
    test::usbdfu_fake_gpio::lines[kRecovery] = {};
    test::usbdfu_fake_gpio::lines[kRecovery].throwOnRequest = true;

    std::string errorMsg;
    auto line = UsbDfuRecovery::requestOutputLine(kRecovery, 1, errorMsg);

    EXPECT_FALSE(static_cast<bool>(line));
    EXPECT_NE(errorMsg.find("Failed to drive GPIO"), std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, RequestOutputLine_SetValueThrows)
{
    test::usbdfu_fake_gpio::lines[kRecovery] = {};
    test::usbdfu_fake_gpio::lines[kRecovery].throwOnSet = true;

    std::string errorMsg;
    auto line = UsbDfuRecovery::requestOutputLine(kRecovery, 1, errorMsg);

    EXPECT_FALSE(static_cast<bool>(line));
    EXPECT_FALSE(errorMsg.empty());
}

TEST_F(UsbDfuRecoveryTest, RequestOutputLine_Success)
{
    test::usbdfu_fake_gpio::lines[kRecovery] = {};

    std::string errorMsg;
    auto line = UsbDfuRecovery::requestOutputLine(kRecovery, 1, errorMsg);

    EXPECT_TRUE(static_cast<bool>(line));
    EXPECT_TRUE(errorMsg.empty());
    EXPECT_TRUE(test::usbdfu_fake_gpio::lines[kRecovery].requested);
    ASSERT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues.size(), 1u);
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues[0], 1);
}

// ============================================================================
// pulseReset tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, PulseReset_ResetLineNotFound)
{
    // No reset GPIO registered
    UsbDfuRecovery dut(makeConfig());
    EXPECT_FALSE(dut.pulseReset());
}

TEST_F(UsbDfuRecoveryTest, PulseReset_SetHighThrows)
{
    // first set_value (low=0) succeeds; second (high=1) throws
    test::usbdfu_fake_gpio::lines[kReset] = {};
    test::usbdfu_fake_gpio::lines[kReset].throwOnSet = true;
    test::usbdfu_fake_gpio::lines[kReset].throwOnSetAfter = 1;

    UsbDfuRecovery dut(makeConfig());
    EXPECT_FALSE(dut.pulseReset());
}

TEST_F(UsbDfuRecoveryTest, PulseReset_Success)
{
    test::usbdfu_fake_gpio::lines[kReset] = {};

    UsbDfuRecovery dut(makeConfig());
    EXPECT_TRUE(dut.pulseReset());

    const auto& vals = test::usbdfu_fake_gpio::lines[kReset].setValues;
    ASSERT_EQ(vals.size(), 2u); // driven low then high
    EXPECT_EQ(vals[0], 0);
    EXPECT_EQ(vals[1], 1);
}

// ============================================================================
// setRecoveryStrap tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, SetRecoveryStrap_SpiMuxNotFound)
{
    // SPI MUX GPIO missing; only recovery and reset present
    test::usbdfu_fake_gpio::lines[kRecovery] = {};
    test::usbdfu_fake_gpio::lines[kReset] = {};

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.setRecoveryStrap(1, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_NE(out["Error"].get<std::string>().find("SPI MUX"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, SetRecoveryStrap_RecoveryGpioNotFound)
{
    // SPI MUX present, recovery GPIO missing
    test::usbdfu_fake_gpio::lines[kSpiMux] = {};
    test::usbdfu_fake_gpio::lines[kReset] = {};

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.setRecoveryStrap(1, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");

    // SPI MUX must have been released on the error path
    EXPECT_TRUE(test::usbdfu_fake_gpio::lines[kSpiMux].released);
}

TEST_F(UsbDfuRecoveryTest, SetRecoveryStrap_PulseResetFails)
{
    // All GPIOs present but reset GPIO missing -> pulseReset fails
    test::usbdfu_fake_gpio::lines[kRecovery] = {};
    test::usbdfu_fake_gpio::lines[kSpiMux] = {};
    // kReset intentionally NOT registered

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.setRecoveryStrap(1, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");

    // Session must be released on failure
    EXPECT_TRUE(test::usbdfu_fake_gpio::lines[kSpiMux].released);
    EXPECT_TRUE(test::usbdfu_fake_gpio::lines[kRecovery].released);
}

TEST_F(UsbDfuRecoveryTest, SetRecoveryStrap_AssertSuccess)
{
    registerAllLines();

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.setRecoveryStrap(1, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");

    // Recovery GPIO driven to 1
    ASSERT_FALSE(test::usbdfu_fake_gpio::lines[kRecovery].setValues.empty());
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues[0], 1);
    // SPI MUX driven to 0
    ASSERT_FALSE(test::usbdfu_fake_gpio::lines[kSpiMux].setValues.empty());
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kSpiMux].setValues[0], 0);
}

TEST_F(UsbDfuRecoveryTest, SetRecoveryStrap_DeassertSuccess)
{
    registerAllLines();

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.setRecoveryStrap(0, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");

    // Recovery GPIO driven to 0
    ASSERT_FALSE(test::usbdfu_fake_gpio::lines[kRecovery].setValues.empty());
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues[0], 0);
}

// ============================================================================
// pollUntil: deadline-inclusive polling shared by the wait helpers
// ============================================================================

TEST_F(UsbDfuRecoveryTest, PollUntil_PollsAtZeroAndEveryIntervalToDeadline)
{
    std::size_t polls = 0;
    std::size_t sleeps = 0;
    g_onSleep = [&sleeps]() { ++sleeps; };
    EXPECT_FALSE(UsbDfuRecovery::pollUntil(2000, 1000, [&polls]() {
        ++polls;
        return false;
    }));
    EXPECT_EQ(polls, 3u); // t = 0, 1 and 2 s
    EXPECT_EQ(sleeps, 2u);
}

TEST_F(UsbDfuRecoveryTest, PollUntil_ZeroTimeoutPollsOnceWithoutSleeping)
{
    std::size_t polls = 0;
    std::size_t sleeps = 0;
    g_onSleep = [&sleeps]() { ++sleeps; };
    EXPECT_FALSE(UsbDfuRecovery::pollUntil(0, 1000, [&polls]() {
        ++polls;
        return false;
    }));
    EXPECT_EQ(polls, 1u);
    EXPECT_EQ(sleeps, 0u);
    EXPECT_TRUE(UsbDfuRecovery::pollUntil(0, 1000, []() { return true; }));
}

TEST_F(UsbDfuRecoveryTest, PollUntil_TimeoutBelowIntervalStillPollsAtDeadline)
{
    std::size_t polls = 0;
    std::size_t sleeps = 0;
    g_onSleep = [&sleeps]() { ++sleeps; };
    // 1 s deadline, 5 s interval: one clamped nap, then the deadline poll.
    EXPECT_FALSE(UsbDfuRecovery::pollUntil(1000, 5000, [&polls]() {
        ++polls;
        return false;
    }));
    EXPECT_EQ(polls, 2u);
    EXPECT_EQ(sleeps, 1u);
}

TEST_F(UsbDfuRecoveryTest, PollUntil_ReturnsAsSoonAsReady)
{
    std::size_t polls = 0;
    std::size_t sleeps = 0;
    g_onSleep = [&sleeps]() { ++sleeps; };
    EXPECT_TRUE(UsbDfuRecovery::pollUntil(10000, 1000,
                                          [&polls]() { return ++polls == 2; }));
    EXPECT_EQ(polls, 2u);
    EXPECT_EQ(sleeps, 1u);
}

// ============================================================================
// waitForDfuEnumeration tests (fake sysfs tree)
// ============================================================================

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_DevicePresent)
{
    fakeUsbDevice("1-1", kVid, kPid);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForDfuEnumeration(out));
    EXPECT_TRUE(out["DfuEnumerated"].get<bool>());
    EXPECT_FALSE(out.contains("ErrorCode"));
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_WrongPid)
{
    fakeUsbDevice("1-1", kVid, "beef");

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.waitForDfuEnumeration(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_FALSE(out["DfuEnumerated"].get<bool>());
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kDfuEnumerationFailed);
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_IgnoresEntriesWithoutIds)
{
    // Hub / interface entries have no idVendor; a matching device elsewhere
    // must still be found.
    std::filesystem::create_directories(sysfsRoot_ / "usb1");
    std::filesystem::create_directories(sysfsRoot_ / "1-1:1.0");
    fakeUsbDevice("1-1.3", kVid, kPid);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForDfuEnumeration(out));
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_MatchesCaseInsensitively)
{
    // sysfs reports lower-case hex; be tolerant of upper-case attributes.
    auto cfg = makeConfig();
    cfg.dfuProductId = "ab12";
    fakeUsbDevice("1-1", kVid, "AB12");

    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForDfuEnumeration(out));
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_SysfsRootMissing)
{
    auto cfg = makeConfig();
    cfg.usbSysfsRoot = (tmpDir_ / "does-not-exist").string();

    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_FALSE(dut.waitForDfuEnumeration(out)); // must not throw
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kDfuEnumerationFailed);
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_TimesOut)
{
    // Empty sysfs tree -> every attempt fails
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.waitForDfuEnumeration(out));
    EXPECT_NE(out["Error"].get<std::string>().find("did not enumerate"),
              std::string::npos);
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kDfuEnumerationFailed);
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_AppearsOnSecondPoll)
{
    // Device shows up "while" the tool sleeps between attempts.
    g_onSleep = [this]() { fakeUsbDevice("1-1", kVid, kPid); };

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForDfuEnumeration(out));
    EXPECT_TRUE(out["DfuEnumerated"].get<bool>());
}

TEST_F(UsbDfuRecoveryTest, WaitForDfuEnumeration_ZeroTimeoutChecksOnce)
{
    auto cfg = makeConfig();
    cfg.dfuEnumerationTimeoutSecs = 0;
    fakeUsbDevice("1-1", kVid, kPid);

    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForDfuEnumeration(out));
}

// ============================================================================
// assertRecoveryMode / deassertRecoveryMode tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, AssertRecoveryMode_GpioFails)
{
    // No GPIOs -> setRecoveryStrap fails -> assertRecoveryMode returns false
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.assertRecoveryMode(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kGpioAssertFailed);
}

TEST_F(UsbDfuRecoveryTest, AssertRecoveryMode_EnumerationTimeout)
{
    registerAllLines();
    // No USB device in the fake sysfs tree

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.assertRecoveryMode(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_FALSE(out["DfuEnumerated"].get<bool>());
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kDfuEnumerationFailed);

    // Straps were driven (recovery=1) and are intentionally left asserted so
    // a manual AssertRecovery can be debugged.
    ASSERT_FALSE(test::usbdfu_fake_gpio::lines[kRecovery].setValues.empty());
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues.back(), 1);
    EXPECT_TRUE(static_cast<bool>(dut.session_));
}

TEST_F(UsbDfuRecoveryTest, AssertRecoveryMode_Success)
{
    makeRecoveryReady();

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.assertRecoveryMode(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
    EXPECT_TRUE(out["DfuEnumerated"].get<bool>());
    EXPECT_FALSE(out.contains("ErrorCode"));
}

TEST_F(UsbDfuRecoveryTest, DeassertRecoveryMode_Fails)
{
    // No GPIOs -> fails
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.deassertRecoveryMode(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kGpioDeassertFailed);
}

TEST_F(UsbDfuRecoveryTest, DeassertRecoveryMode_ReleasesSession)
{
    makeRecoveryReady();

    UsbDfuRecovery dut(makeConfig());
    // Assert first so the session holds lines
    {
        nlohmann::json tmp;
        ASSERT_TRUE(dut.assertRecoveryMode(tmp));
    }
    nlohmann::json out;
    EXPECT_TRUE(dut.deassertRecoveryMode(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");

    // After deassert the session must have been released
    EXPECT_FALSE(static_cast<bool>(dut.session_));
}

// ============================================================================
// runDfuDownload / runDfuUtil tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_FileNotFound)
{
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::FirmwareImage,
                                    "/nonexistent/firmware.bin", out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_NE(out["Error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_DfuUtilFails)
{
    auto fwPath = tmpDir_ / "fw.bin";
    touchFile(fwPath);

    // Simulate dfu-util returning non-zero exit code
    g_waitpidStatus = (1 << 8); // WEXITSTATUS = 1

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::FirmwareImage,
                                    fwPath.string(), out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
}

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_ForkFails)
{
    auto fwPath = tmpDir_ / "fw.bin";
    touchFile(fwPath);

    g_forkRet = -1; // fork() fails

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::FirmwareImage,
                                    fwPath.string(), out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
}

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_Success)
{
    auto fwPath = tmpDir_ / "fw.bin";
    touchFile(fwPath);
    // g_waitpidStatus = 0 -> WEXITSTATUS=0 (success, default)

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::FirmwareImage,
                                   fwPath.string(), out));
}

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_BundleStageUsesDefaultAlt)
{
    auto blob = tmpDir_ / "blob.bin";
    touchFile(blob);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::BundleStage,
                                   blob.string(), out));
    EXPECT_EQ(dut.lastCommand(), "/fake/dfu-util -D " + blob.string());
}

TEST_F(UsbDfuRecoveryTest, RunDfuDownload_FirmwareImageUsesAltAndReset)
{
    auto fwPath = tmpDir_ / "fw.bin";
    touchFile(fwPath);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.runDfuDownload(UsbDfuRecovery::DfuTransfer::FirmwareImage,
                                   fwPath.string(), out));
    EXPECT_EQ(dut.lastCommand(),
              "/fake/dfu-util -a recovery_both -R -D " + fwPath.string());
}

TEST_F(UsbDfuRecoveryTest, TransferParams_DeriveFromConfig)
{
    auto cfg = makeConfig();
    cfg.dfuAltSetting = "recovery_cs0";
    cfg.resetAfterFlash = false;
    cfg.bundleStepTimeoutSecs = 7;
    cfg.flashTimeoutSecs = 9;
    UsbDfuRecovery dut(cfg);

    auto bundle = dut.transferParams(UsbDfuRecovery::DfuTransfer::BundleStage);
    EXPECT_EQ(bundle.label, "Bundle binary");
    EXPECT_TRUE(bundle.dfuUtilArgs.empty());
    EXPECT_EQ(bundle.timeoutSecs, 7);

    auto fw = dut.transferParams(UsbDfuRecovery::DfuTransfer::FirmwareImage);
    EXPECT_EQ(fw.label, "Firmware image");
    EXPECT_EQ(fw.dfuUtilArgs, (std::vector<std::string>{"-a", "recovery_cs0"}));
    EXPECT_EQ(fw.timeoutSecs, 9);
}

TEST_F(UsbDfuRecoveryTest, RunDfuUtil_RecordsLastCommand)
{
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(
        dut.runDfuUtil({"-a", "recovery_cs0", "-D", "/x.bin"}, 30, out));
    EXPECT_EQ(dut.lastCommand(), "/fake/dfu-util -a recovery_cs0 -D /x.bin");
}

// ============================================================================
// runPreliminaryBundle tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, RunPreliminaryBundle_EmptyBundle)
{
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.runPreliminaryBundle({}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
}

TEST_F(UsbDfuRecoveryTest, RunPreliminaryBundle_FirstBinaryMissing)
{
    // Do NOT create the binary file
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runPreliminaryBundle({tmpDir_ / "missing.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["FailedStep"].get<int>(), 1);
    EXPECT_EQ(out["FailedBinary"].get<std::string>(), "missing.bin");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kBundleSendFailed);
}

TEST_F(UsbDfuRecoveryTest, RunPreliminaryBundle_SecondBinaryFails)
{
    touchFile(tmpDir_ / "step1.bin");
    touchFile(tmpDir_ / "step2.bin");
    g_waitpidFailOnCall = 2; // second dfu-util invocation fails

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runPreliminaryBundle(
        {tmpDir_ / "step1.bin", tmpDir_ / "step2.bin"}, out));
    EXPECT_EQ(out["FailedStep"].get<int>(), 2);
    EXPECT_EQ(out["FailedBinary"].get<std::string>(), "step2.bin");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kBundleSendFailed);
}

TEST_F(UsbDfuRecoveryTest, RunPreliminaryBundle_Success)
{
    touchFile(tmpDir_ / "a.bin");
    touchFile(tmpDir_ / "b.bin");

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(
        dut.runPreliminaryBundle({tmpDir_ / "a.bin", tmpDir_ / "b.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
    EXPECT_EQ(g_waitpidCallCount, 2u);
}

// ============================================================================
// prepareFlashImage tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, PrepareFlashImage_MissingImage)
{
    UsbDfuRecovery::StagedImage staged;
    nlohmann::json out;
    EXPECT_FALSE(UsbDfuRecovery::prepareFlashImage("/nonexistent/fw.bin", 16,
                                                   staged, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_FALSE(staged.temporary);
}

TEST_F(UsbDfuRecoveryTest, PrepareFlashImage_LengthZeroSendsWholeFile)
{
    auto fwPath = tmpDir_ / "fw.bin";
    writeBytes(fwPath, 64);

    UsbDfuRecovery::StagedImage staged;
    nlohmann::json out;
    EXPECT_TRUE(
        UsbDfuRecovery::prepareFlashImage(fwPath.string(), 0, staged, out));
    EXPECT_EQ(staged.path, fwPath);
    EXPECT_EQ(staged.sizeBytes, 64u);
    EXPECT_FALSE(staged.temporary);
}

TEST_F(UsbDfuRecoveryTest, PrepareFlashImage_ShortImageUnchanged)
{
    auto fwPath = tmpDir_ / "fw.bin";
    writeBytes(fwPath, 16);

    UsbDfuRecovery::StagedImage staged;
    nlohmann::json out;
    EXPECT_TRUE(
        UsbDfuRecovery::prepareFlashImage(fwPath.string(), 64, staged, out));
    EXPECT_EQ(staged.path, fwPath);
    EXPECT_EQ(staged.sizeBytes, 16u);
    EXPECT_FALSE(staged.temporary);
    EXPECT_FALSE(std::filesystem::exists(tmpDir_ / "fw.bin.dfu"));
}

TEST_F(UsbDfuRecoveryTest, PrepareFlashImage_TruncatesLongImage)
{
    auto fwPath = tmpDir_ / "fw.bin";
    writeBytes(fwPath, 64);
    const auto stagedPath = tmpDir_ / "fw.bin.dfu";

    {
        UsbDfuRecovery::StagedImage staged;
        nlohmann::json out;
        EXPECT_TRUE(UsbDfuRecovery::prepareFlashImage(fwPath.string(), 16,
                                                      staged, out));
        EXPECT_EQ(staged.path, stagedPath);
        EXPECT_EQ(staged.sizeBytes, 16u);
        EXPECT_TRUE(staged.temporary);
        EXPECT_EQ(std::filesystem::file_size(stagedPath), 16u);
        // Original image untouched
        EXPECT_EQ(std::filesystem::file_size(fwPath), 64u);
    }
    // Staged copy removed when the StagedImage goes out of scope
    EXPECT_FALSE(std::filesystem::exists(stagedPath));
}

// ============================================================================
// flashFirmware tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, FlashFirmware_ImageNotFound)
{
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.flashFirmware("/nonexistent/hmc_fw.bin", out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kFirmwareFlashFailed);
}

TEST_F(UsbDfuRecoveryTest, FlashFirmware_DfuUtilFails)
{
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);
    g_waitpidStatus = (1 << 8); // WEXITSTATUS=1

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.flashFirmware(fwPath.string(), out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kFirmwareFlashFailed);
}

TEST_F(UsbDfuRecoveryTest, FlashFirmware_Success)
{
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    writeBytes(fwPath, 32);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.flashFirmware(fwPath.string(), out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
    EXPECT_EQ(out["DfuAlt"].get<std::string>(), "recovery_both");
    EXPECT_EQ(out["FlashedBytes"].get<uint64_t>(), 32u);
}

TEST_F(UsbDfuRecoveryTest, FlashFirmware_UsesConfiguredAltAndReset)
{
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    auto cfg = makeConfig();
    cfg.dfuAltSetting = "recovery_cs0";
    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_TRUE(dut.flashFirmware(fwPath.string(), out));
    EXPECT_EQ(dut.lastCommand(),
              "/fake/dfu-util -a recovery_cs0 -R -D " + fwPath.string());
    EXPECT_EQ(out["DfuAlt"].get<std::string>(), "recovery_cs0");
}

TEST_F(UsbDfuRecoveryTest, FlashFirmware_NoResetFlag)
{
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    auto cfg = makeConfig();
    cfg.resetAfterFlash = false;
    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_TRUE(dut.flashFirmware(fwPath.string(), out));
    EXPECT_EQ(dut.lastCommand().find(" -R"), std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, FlashFirmware_TruncatesAndCleansUp)
{
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    writeBytes(fwPath, 64);
    const auto stagedPath = tmpDir_ / "hmc_fw.bin.dfu";

    auto cfg = makeConfig();
    cfg.flashLengthBytes = 16;
    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_TRUE(dut.flashFirmware(fwPath.string(), out));
    EXPECT_EQ(out["FlashedBytes"].get<uint64_t>(), 16u);
    // dfu-util was pointed at the staged copy ...
    EXPECT_NE(dut.lastCommand().find(stagedPath.string()), std::string::npos);
    // ... which is removed afterwards; the original is untouched.
    EXPECT_FALSE(std::filesystem::exists(stagedPath));
    EXPECT_EQ(std::filesystem::file_size(fwPath), 64u);
}

// ============================================================================
// dfu-util output capture and recovery U-Boot DFU wait
// ============================================================================

TEST_F(UsbDfuRecoveryTest, RunDfuUtil_ReportsExitCodeAndOutputOnFailure)
{
    g_waitpidStatus = (3 << 8); // dfu-util exits 3
    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.runDfuUtil({"-l"}, 30, out));
    EXPECT_EQ(out["DfuUtilExitCode"].get<int>(), 3);
    EXPECT_TRUE(out.contains("DfuUtilOutput")); // empty under the fork wrap
    EXPECT_NE(out["Error"].get<std::string>().find("dfu-util failed"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, WaitForRecoveryUBoot_TimesOut)
{
    auto cfg = makeConfig();
    cfg.uBootDfuTimeoutSecs = 2; // polls at 0, 1 and 2 s; the wrapped fork
                                 // yields an empty dfu-util -l listing
    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_FALSE(dut.waitForRecoveryUBoot(out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kRecoveryUBootDfuTimeout);
    EXPECT_EQ(g_waitpidCallCount, 3u);
}

TEST_F(UsbDfuRecoveryTest, WaitForRecoveryUBoot_DisabledReturnsImmediately)
{
    UsbDfuRecovery dut(makeConfig()); // uBootDfuTimeoutSecs = 0
    nlohmann::json out;
    EXPECT_TRUE(dut.waitForRecoveryUBoot(out));
    EXPECT_FALSE(out.contains("Error"));
    EXPECT_EQ(g_waitpidCallCount, 0u);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_UBootNeverAppears_Deasserts)
{
    makeRecoveryReady();
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    auto cfg = makeConfig();
    cfg.uBootDfuTimeoutSecs = 2;
    UsbDfuRecovery dut(cfg);
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Steps"]["PreliminaryBundle"].get<std::string>(),
              "Successful");
    EXPECT_EQ(out["Steps"]["RecoveryUBoot"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kRecoveryUBootDfuTimeout);
    // The SPI image was never sent and the strap was released best-effort.
    EXPECT_EQ(g_waitpidCallCount, 4u); // bundle stage + three dfu-util -l polls
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues.back(), 0);
}

// ============================================================================
// performFullRecovery tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_PreflightImageMissing)
{
    makeRecoveryReady();
    touchFile(tmpDir_ / "bundle.bin");

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery("/nonexistent/fw.bin",
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["Steps"]["Preflight"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kFirmwareFlashFailed);
    // No GPIO was touched
    EXPECT_FALSE(test::usbdfu_fake_gpio::lines[kRecovery].requested);
    EXPECT_FALSE(test::usbdfu_fake_gpio::lines[kSpiMux].requested);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_PreflightBundleMissing)
{
    makeRecoveryReady();
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "missing.bin"}, out));
    EXPECT_EQ(out["Steps"]["Preflight"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kBundleSendFailed);
    EXPECT_FALSE(test::usbdfu_fake_gpio::lines[kRecovery].requested);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_PreflightEmptyBundle)
{
    makeRecoveryReady();
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(), {}, out));
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kInvalidConfiguration);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_AssertFails)
{
    // Files present but no GPIOs -> assertRecoveryMode fails
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["Steps"]["Preflight"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["AssertRecovery"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kGpioAssertFailed);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_EnumerationFails_Deasserts)
{
    registerAllLines(); // GPIOs present, but no USB device appears
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Steps"]["AssertRecovery"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kDfuEnumerationFailed);

    // Best-effort cleanup: recovery strap driven 1 then back to 0, lines freed
    const auto& vals = test::usbdfu_fake_gpio::lines[kRecovery].setValues;
    ASSERT_EQ(vals.size(), 2u);
    EXPECT_EQ(vals[0], 1);
    EXPECT_EQ(vals[1], 0);
    EXPECT_FALSE(static_cast<bool>(dut.session_));
    // dfu-util never ran
    EXPECT_EQ(g_waitpidCallCount, 0u);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_BundleFails)
{
    makeRecoveryReady();
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);
    g_waitpidFailOnCall = 1; // first dfu-util call (bundle) fails

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["Steps"]["AssertRecovery"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["PreliminaryBundle"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kBundleSendFailed);

    // Cleanup deassert happened: recovery strap 1 -> 0
    const auto& vals = test::usbdfu_fake_gpio::lines[kRecovery].setValues;
    ASSERT_EQ(vals.size(), 2u);
    EXPECT_EQ(vals.back(), 0);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_FlashFails)
{
    makeRecoveryReady();
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    touchFile(fwPath);
    g_waitpidFailOnCall = 2; // bundle (call 1) ok, flash (call 2) fails

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_FALSE(dut.performFullRecovery(fwPath.string(),
                                         {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["Steps"]["PreliminaryBundle"].get<std::string>(),
              "Successful");
    EXPECT_EQ(out["Steps"]["FlashFirmware"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kFirmwareFlashFailed);

    // Cleanup deassert happened
    EXPECT_EQ(test::usbdfu_fake_gpio::lines[kRecovery].setValues.back(), 0);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_Success)
{
    makeRecoveryReady();
    touchFile(tmpDir_ / "bundle.bin");
    auto fwPath = tmpDir_ / "hmc_fw.bin";
    writeBytes(fwPath, 48);

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.performFullRecovery(fwPath.string(),
                                        {tmpDir_ / "bundle.bin"}, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
    EXPECT_FALSE(out.contains("ErrorCode"));
    EXPECT_EQ(out["Steps"]["Preflight"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["AssertRecovery"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["DfuEnumeration"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["PreliminaryBundle"].get<std::string>(),
              "Successful");
    EXPECT_EQ(out["Steps"]["FlashFirmware"].get<std::string>(), "Successful");
    EXPECT_EQ(out["Steps"]["DeassertRecovery"].get<std::string>(),
              "Successful");
    EXPECT_EQ(out["DfuAlt"].get<std::string>(), "recovery_both");
    EXPECT_EQ(out["FlashedBytes"].get<uint64_t>(), 48u);
    // bundle + flash
    EXPECT_EQ(g_waitpidCallCount, 2u);
    EXPECT_FALSE(static_cast<bool>(dut.session_));
}

// ============================================================================
// resolvePackageComponents tests (PLDM-extracted package layout)
// ============================================================================

namespace
{
/// Create <root>/<id decimal>/<name> for every component in the table.
std::filesystem::path makePackage(const std::filesystem::path& root)
{
    for (const auto& c : usbdfu::componentMap)
    {
        auto dir = root / std::to_string(c.id);
        std::filesystem::create_directories(dir);
        writeBytes(dir / (std::string(c.name) + ".bin"), 4);
    }
    return root;
}
} // namespace

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_Complete)
{
    auto pkg = makePackage(tmpDir_ / "pkg");

    PackageContents contents;
    nlohmann::json out;
    ASSERT_TRUE(resolvePackageComponents(pkg, contents, out));
    EXPECT_FALSE(out.contains("ErrorCode"));

    // Eleven bundle stages in table order (incl. optional Zephyr SSP/TSP),
    // then the SPI image.
    ASSERT_EQ(contents.bundle.size(), usbdfu::componentMap.size() - 1);
    EXPECT_EQ(contents.bundle.front(), pkg / "1" / "Caliptra_FW.bin");
    EXPECT_EQ(contents.bundle[8], pkg / "9" / "U-Boot.bin");
    EXPECT_EQ(contents.bundle.back(), pkg / "11" / "Zephyr_TSP.bin");
    EXPECT_EQ(contents.firmwareImage, pkg / "16" / "HMC_SPI_Image.bin");
}

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_OptionalZephyrAbsent)
{
    // A 9-stage package (no SSP/TSP components) is complete as well.
    auto pkg = makePackage(tmpDir_ / "pkg");
    std::filesystem::remove_all(pkg / "10");
    std::filesystem::remove_all(pkg / "11");

    PackageContents contents;
    nlohmann::json out;
    ASSERT_TRUE(resolvePackageComponents(pkg, contents, out));
    EXPECT_FALSE(out.contains("ErrorCode"));
    ASSERT_EQ(contents.bundle.size(), 9u);
    EXPECT_EQ(contents.bundle.back(), pkg / "9" / "U-Boot.bin");
    EXPECT_EQ(contents.firmwareImage, pkg / "16" / "HMC_SPI_Image.bin");
}

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_MissingComponent)
{
    auto pkg = makePackage(tmpDir_ / "pkg");
    std::filesystem::remove_all(pkg / "7"); // BL31 absent

    PackageContents contents;
    nlohmann::json out;
    EXPECT_FALSE(resolvePackageComponents(pkg, contents, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Failed");
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kPackageIncomplete);
    EXPECT_NE(out["Error"].get<std::string>().find("0x7 (BL31)"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_EmptyComponentDir)
{
    auto pkg = makePackage(tmpDir_ / "pkg");
    std::filesystem::remove(pkg / "16" / "HMC_SPI_Image.bin"); // dir stays

    PackageContents contents;
    nlohmann::json out;
    EXPECT_FALSE(resolvePackageComponents(pkg, contents, out));
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kPackageIncomplete);
    EXPECT_NE(out["Error"].get<std::string>().find("HMC_SPI_Image"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_IgnoresSubdirectories)
{
    auto pkg = makePackage(tmpDir_ / "pkg");
    // A stray directory inside a component dir must not be taken as the file.
    std::filesystem::create_directories(pkg / "1" / "junk");

    PackageContents contents;
    nlohmann::json out;
    ASSERT_TRUE(resolvePackageComponents(pkg, contents, out));
    EXPECT_EQ(contents.bundle.front(), pkg / "1" / "Caliptra_FW.bin");
}

TEST_F(UsbDfuRecoveryTest, ResolvePackageComponents_PackageDirMissing)
{
    PackageContents contents;
    nlohmann::json out;
    EXPECT_FALSE(
        resolvePackageComponents(tmpDir_ / "nonexistent", contents, out));
    EXPECT_EQ(out["ErrorCode"].get<uint8_t>(), kPackageIncomplete);
}

TEST_F(UsbDfuRecoveryTest, PerformFullRecovery_FromPackage_Success)
{
    makeRecoveryReady();
    auto pkg = makePackage(tmpDir_ / "pkg");
    PackageContents contents;
    nlohmann::json err;
    ASSERT_TRUE(resolvePackageComponents(pkg, contents, err));

    UsbDfuRecovery dut(makeConfig());
    nlohmann::json out;
    EXPECT_TRUE(dut.performFullRecovery(contents.firmwareImage.string(),
                                        contents.bundle, out));
    EXPECT_EQ(out["Status"].get<std::string>(), "Successful");
    // 9 bundle stages + 1 flash
    EXPECT_EQ(g_waitpidCallCount, usbdfu::componentMap.size());
    EXPECT_NE(dut.lastCommand().find("16/HMC_SPI_Image.bin"),
              std::string::npos);
}

// ============================================================================
// loadBundleManifest tests
// ============================================================================

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_NotFound)
{
    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest("/nonexistent/bundle.json", delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_InvalidJson)
{
    auto p = tmpDir_ / "bad.json";
    writeFile(p, "{ NOT VALID JSON");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("parse error"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_MissingBinariesKey)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({ "step_delay_secs": 5 })");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("binaries"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_EmptyBinariesArray)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({ "binaries": [] })");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("no binaries"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_PathTraversalRejected)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({ "binaries": ["../../../etc/passwd"] })");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("plain filename"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_SlashInNameRejected)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({ "binaries": ["subdir/file.bin"] })");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    EXPECT_TRUE(bins.empty());
    EXPECT_NE(out["Error"].get<std::string>().find("plain filename"),
              std::string::npos);
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_ValidManifest)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({
        "binaries": ["boot1.bin", "boot2.bin", "boot3.bin"],
        "step_delay_secs": 7
    })");

    nlohmann::json out;
    int delay = 10;
    auto bins = loadBundleManifest(p.string(), delay, out);

    ASSERT_EQ(bins.size(), 3u);
    EXPECT_EQ(bins[0], "boot1.bin");
    EXPECT_EQ(bins[1], "boot2.bin");
    EXPECT_EQ(bins[2], "boot3.bin");
    EXPECT_EQ(delay, 7);
    EXPECT_FALSE(out.contains("Error"));
}

TEST_F(UsbDfuRecoveryTest, LoadBundleManifest_DefaultDelay)
{
    auto p = tmpDir_ / "manifest.json";
    writeFile(p, R"({ "binaries": ["only.bin"] })");

    nlohmann::json out;
    int delay = 42; // should be reset to the documented default when absent
    auto bins = loadBundleManifest(p.string(), delay, out);

    ASSERT_EQ(bins.size(), 1u);
    EXPECT_EQ(delay, usbdfu::timing::bundleStepDelaySecs);
}
