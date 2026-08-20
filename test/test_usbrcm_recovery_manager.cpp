/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../fw-status/force_recovery/usbrcm_recovery_manager.hpp"
#include "force_recovery.hpp"
#include "get_recovery_status.hpp"

#include <time.h>

#include <nlohmann/json.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include "gtest/gtest.h"

namespace
{

using ::testing::NiceMock;

class UsbRcmManagerTest : public ::testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    nvidia::recovery::USBRCMRecoveryManager manager{
        bus, "chassis0", "/nvidia/chassis0", RecoveryPinConfig{}};

    void SetUp() override
    {
        test::fw_status_fake_force_recovery::reset();
        test::fw_status_fake_usb_recovery::reset();
    }
};

// Helper: one device payload with "In Recovery" status.
nlohmann::json allInRecovery()
{
    auto devices = nlohmann::json::array();
    devices.push_back({{"Recovery Status", "In Recovery"}});
    return devices;
}

} // namespace

// ---- areDevicesInRecovery() branches (exercised via setRecoveryMode) ----

// getRecoveryStatus returns failure → areDevicesInRecovery false all 3 retries
TEST_F(UsbRcmManagerTest, RecoveryStatusCallFails)
{
    test::fw_status_fake_usb_recovery::fallbackSuccess = false;
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);
}

// getRecoveryStatus returns non-array
TEST_F(UsbRcmManagerTest, RecoveryStatusNotArray)
{
    // 1st retry: non-array → false; 2nd: in-recovery → true; restores OK
    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json{{"key", "value"}});
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// getRecoveryStatus returns empty array
TEST_F(UsbRcmManagerTest, RecoveryStatusEmptyArray)
{
    test::fw_status_fake_usb_recovery::pushReply(true, nlohmann::json::array());
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// Device present but missing "Recovery Status" key
TEST_F(UsbRcmManagerTest, DeviceMissingRecoveryStatusKey)
{
    auto devices = nlohmann::json::array();
    devices.push_back({{"Other Key", "value"}});
    test::fw_status_fake_usb_recovery::pushReply(true, devices);
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// Device has "Recovery Status" != "In Recovery"
TEST_F(UsbRcmManagerTest, DeviceNotInRecovery)
{
    auto devices = nlohmann::json::array();
    devices.push_back(
        {{"Recovery Status", "Normal"}, {"USB Port Path", "/dev/bus/1"}});
    test::fw_status_fake_usb_recovery::pushReply(true, devices);
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// All devices in recovery on first attempt
TEST_F(UsbRcmManagerTest, AllDevicesInRecoveryFirstAttempt)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    EXPECT_NO_THROW(manager.setRecoveryMode());
    EXPECT_EQ(test::fw_status_fake_force_recovery::forceCallCount, 1);
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCallCount, 1);
}

// ---- performForceRecovery() branches ----

// forceRecoveryMode result missing "Status" key → throws
TEST_F(UsbRcmManagerTest, ForceResultMissingStatusKey)
{
    test::fw_status_fake_force_recovery::forceResult = nlohmann::json::object();
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCallCount, 0);
}

// forceRecoveryMode "Status" != "Successful" → throws
TEST_F(UsbRcmManagerTest, ForceResultStatusFailed)
{
    test::fw_status_fake_force_recovery::forceResult = {{"Status", "Failed"},
                                                        {"Error", "GPIO err"}};
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCallCount, 0);
}

// No devices ever enter recovery → throws after maxRetries
TEST_F(UsbRcmManagerTest, DevicesNeverEnterRecovery)
{
    // fallbackPayload is empty array → areDevicesInRecovery returns false
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCallCount, 1);
}

// GPIO restore result missing "Status" key (logs error, no throw if in
// recovery)
TEST_F(UsbRcmManagerTest, GpioRestoreMissingStatusKey)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    test::fw_status_fake_force_recovery::defaultResult =
        nlohmann::json::object();
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// GPIO restore "Status" != "Successful" (logs error, no throw if in recovery)
TEST_F(UsbRcmManagerTest, GpioRestoreStatusFailed)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    test::fw_status_fake_force_recovery::defaultResult = {{"Status", "Failed"},
                                                          {"Error", "restore"}};
    EXPECT_NO_THROW(manager.setRecoveryMode());
}

// Two devices both in recovery → exercises for-loop back-edge in
// areDevicesInRecovery (second iteration of the device range-for).
TEST_F(UsbRcmManagerTest, TwoDevicesInRecoveryCoversDeviceForLoopBackEdge)
{
    auto devices = nlohmann::json::array();
    devices.push_back({{"Recovery Status", "In Recovery"}});
    devices.push_back({{"Recovery Status", "In Recovery"}});
    test::fw_status_fake_usb_recovery::fallbackPayload = devices;
    EXPECT_NO_THROW(manager.setRecoveryMode());
    EXPECT_EQ(test::fw_status_fake_force_recovery::forceCallCount, 1);
}

// Set a recovery-complete callback so that if(onRecoveryComplete) takes the
// true branch in RecoveryModeManagerBase::setRecoveryMode().
TEST_F(UsbRcmManagerTest, RecoveryCompleteCallbackIsInvoked)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = allInRecovery();
    bool called = false;
    manager.setRecoveryCompleteCallback([&called]() { called = true; });
    EXPECT_NO_THROW(manager.setRecoveryMode());
    EXPECT_TRUE(called);
}

extern "C" int __wrap_nanosleep(const struct timespec*, struct timespec*)
{
    return 0;
}

extern "C" int __wrap_clock_nanosleep(clockid_t, int, const struct timespec*,
                                      struct timespec*)
{
    return 0;
}
