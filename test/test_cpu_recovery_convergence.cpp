/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dbusutils.hpp"
#include "get_recovery_status.hpp"

#include <unistd.h>

#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "gtest/gtest.h"

namespace
{

struct FakeUdevState
{
    int pipeFds[2] = {-1, -1};
    bool devicePending = false;
    std::array<char, 16> action{};
    std::optional<std::string> devpath;
    int deviceUnrefCount = 0;
};

FakeUdevState fakeUdev;

void resetFakeUdev()
{
    fakeUdev = {};
    if (pipe(fakeUdev.pipeFds) != 0)
    {
        throw std::runtime_error("failed to create fake udev pipe");
    }
}

void queueUdevDevice(const std::string& action, std::string devpath)
{
    fakeUdev.devicePending = true;
    std::snprintf(fakeUdev.action.data(), fakeUdev.action.size(), "%s",
                  action.c_str());
    fakeUdev.devpath = std::move(devpath);
}

} // namespace

extern "C" struct udev* __wrap_udev_new()
{
    return reinterpret_cast<struct udev*>(0x1);
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_new_from_netlink(struct udev*, const char*)
{
    return reinterpret_cast<struct udev_monitor*>(0x2);
}

extern "C" int __wrap_udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor*, const char*, const char*)
{
    return 0;
}

extern "C" int __wrap_udev_monitor_enable_receiving(struct udev_monitor*)
{
    return 0;
}

extern "C" int __wrap_udev_monitor_get_fd(struct udev_monitor*)
{
    return fakeUdev.pipeFds[0];
}

extern "C" struct udev_device*
    __wrap_udev_monitor_receive_device(struct udev_monitor*)
{
    if (!fakeUdev.devicePending)
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev_device*>(0x3);
}

extern "C" const char* __wrap_udev_device_get_action(struct udev_device*)
{
    return fakeUdev.action.data();
}

extern "C" const char* __wrap_udev_device_get_devpath(struct udev_device*)
{
    if (!fakeUdev.devpath)
    {
        return nullptr;
    }
    return fakeUdev.devpath->c_str();
}

extern "C" struct udev* __wrap_udev_unref(struct udev* context)
{
    return context;
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_unref(struct udev_monitor* monitor)
{
    return monitor;
}

extern "C" struct udev_device*
    __wrap_udev_device_unref(struct udev_device* device)
{
    ++fakeUdev.deviceUnrefCount;
    fakeUdev.devicePending = false;
    std::snprintf(fakeUdev.action.data(), fakeUdev.action.size(), "%s",
                  "remove");
    return device;
}

#define private public
#define protected public
#pragma GCC push_options
#pragma GCC optimize("no-inline")
#include "../fw-status/mctp_discovery_resource.cpp"
#include "../fw-status/udev_monitor.cpp"
#include "../fw-status/usb_rcm_resource.cpp"
#pragma GCC pop_options
#undef protected
#undef private

void BaseResource::commitRecoveryModeError([[maybe_unused]] uint8_t eid)
{}

namespace
{

class CPURecoveryConvergenceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetFakeUdev();
    }

    void TearDown() override
    {
        if (fakeUdev.pipeFds[0] >= 0)
        {
            close(fakeUdev.pipeFds[0]);
        }
        if (fakeUdev.pipeFds[1] >= 0)
        {
            close(fakeUdev.pipeFds[1]);
        }
    }

    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
};

TEST_F(CPURecoveryConvergenceTest, ResourceRegistersConfiguredUSBPortCallback)
{
    auto event = sdeventplus::Event::get_default();
    auto monitor = std::make_shared<UdevMonitor>(event);

    USBRcmResource resource(bus, "/xyz/openbmc_project/software/fmc", 14,
                            "1-2.3", "/xyz/openbmc_project/software/fws",
                            monitor);

    ASSERT_EQ(monitor->callbacks.size(), 1);
    EXPECT_TRUE(monitor->callbacks.contains("1-2.3"));
}

TEST_F(CPURecoveryConvergenceTest, AddActionSurvivesDeviceUnref)
{
    auto event = sdeventplus::Event::get_default();
    UdevMonitor monitor(event);
    int callbackCount = 0;
    monitor.registerCallback("1-2.3", [&callbackCount]() { ++callbackCount; });

    queueUdevDevice("add", "/sys/devices/platform/usb1/1-2/1-2.3");
    monitor.handleUdevEvent();

    EXPECT_EQ(fakeUdev.deviceUnrefCount, 1);
    EXPECT_EQ(std::string(fakeUdev.action.data()), "remove");
    EXPECT_EQ(callbackCount, 1);
}

} // namespace
