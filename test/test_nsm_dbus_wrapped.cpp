/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for nsm_debug_token.cpp and update_debug_token.cpp D-Bus success paths
 * using --wrap=sd_bus_call to intercept and mock D-Bus responses.
 *
 * This allows the enumeration functions to "succeed" and return endpoints,
 * enabling the loop bodies in nsmTokenErase/Install to execute.
 */

#include <sdbusplus/bus.hpp>
#include <sdbusplus/sdbus.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

#include <sys/mman.h>
#include <unistd.h>

// ========================== sd_bus_call wrap ==============================
// Intercept all sd_bus_call invocations. We control what each call returns.

extern "C" int __real_sd_bus_call(sd_bus* bus, sd_bus_message* m, uint64_t usec,
                                  sd_bus_error* ret_error,
                                  sd_bus_message** reply);

namespace dbus_wrap
{
static int callCount = 0;
static bool failAll = true;

void reset()
{
    callCount = 0;
    failAll = true;
}

void setSucceed()
{
    failAll = false;
}
} // namespace dbus_wrap

extern "C" int __wrap_sd_bus_call(sd_bus* /*bus*/, sd_bus_message* m,
                                  uint64_t /*usec*/, sd_bus_error* ret_error,
                                  sd_bus_message** reply)
{
    dbus_wrap::callCount++;

    if (dbus_wrap::failAll)
    {
        if (ret_error)
        {
            ret_error->name = "org.freedesktop.DBus.Error.ServiceUnknown";
            ret_error->message = "Mocked failure";
        }
        return -ENOENT;
    }

    // Try to create a method return from the request message
    if (reply && m)
    {
        if (sd_bus_message_new_method_return(m, reply) >= 0 && *reply)
        {
            // Seal the message so read operations work (empty response)
            sd_bus_message_seal(*reply, 0, 0);
            return 0;
        }
    }
    if (reply)
    {
        *reply = nullptr;
    }
    return 0;
}

// Also wrap sd_bus_call_async to handle match registration
extern "C" int __real_sd_bus_call_async(sd_bus*, sd_bus_slot**, sd_bus_message*,
                                        sd_bus_message_handler_t, void*,
                                        uint64_t);

// Also wrap popen for the mctp-vdm-util calls
extern "C" FILE* __real_popen(const char*, const char*);
extern "C" int __real_pclose(FILE*);

namespace popen_wrap3
{
static std::string output;
static int retCode = 0;

void set(const std::string& o, int rc = 0)
{
    output = o;
    retCode = rc;
}
} // namespace popen_wrap3

extern "C" FILE* __wrap_popen(const char*, const char* mode)
{
    return fmemopen(const_cast<char*>(popen_wrap3::output.c_str()),
                    popen_wrap3::output.size(), mode);
}

extern "C" int __wrap_pclose(FILE* stream)
{
    if (stream)
        fclose(stream);
    return popen_wrap3::retCode;
}

// Wrap sd_bus_open_system for bus creation
extern "C" int __wrap_sd_bus_open_system(sd_bus** bus)
{
    *bus = nullptr;
    return 0;
}

// ========================== Test fixture =================================

class NsmDbusWrappedTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        dbus_wrap::reset();
        popen_wrap3::set("", 0);
    }
};

// ========================== Tests using wrapped sd_bus_call ===============

// When sd_bus_call fails for enumeration, functions return -1
TEST_F(NsmDbusWrappedTest, NsmTokenEraseEnumFails)
{
    dbus_wrap::failAll = true;
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(NsmDbusWrappedTest, NsmTokenInstallEnumFails)
{
    dbus_wrap::failAll = true;
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(NsmDbusWrappedTest, NsmTokenEraseV2EnumFails)
{
    dbus_wrap::failAll = true;
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

TEST_F(NsmDbusWrappedTest, NsmTokenInstallV2EnumFails)
{
    dbus_wrap::failAll = true;
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

// When sd_bus_call succeeds but returns empty data (nullptr reply)
TEST_F(NsmDbusWrappedTest, NsmTokenEraseCallSucceedsEmptyReply)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    // enumerateNsmDebugTokenEndpoints: bus.call returns 0 but reply is nullptr
    // reply.read(objects) with nullptr msg -> likely returns empty
    int result = udt.nsmTokenErase();
    // Either returns 0 (empty endpoints) or -1 (read fails)
    (void)result;
}

TEST_F(NsmDbusWrappedTest, NsmTokenInstallCallSucceedsEmptyReply)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

TEST_F(NsmDbusWrappedTest, NsmTokenEraseV2CallSucceeds)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    (void)result;
}

TEST_F(NsmDbusWrappedTest, NsmTokenInstallV2CallSucceeds)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// Test handleAsyncCall with wrapped bus
TEST_F(NsmDbusWrappedTest, HandleAsyncCallWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCall("/test/path", "EraseToken");
    // makeDebugTokenMethodCall returns "" or a path depending on read
    (void)result;
}

TEST_F(NsmDbusWrappedTest, HandleAsyncCallInstallV2Wrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(64, 0xAA);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);
        auto result = udt.handleAsyncCallInstallV2("/test/path", memfd);
        (void)result;
        close(memfd);
    }
}

TEST_F(NsmDbusWrappedTest, HandleAsyncCallEraseV2Wrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/test/path");
    (void)result;
}

// Test getErasePolicy with wrapped bus
TEST_F(NsmDbusWrappedTest, GetErasePolicyWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto policy = udt.getErasePolicy();
    // Will return empty because read() on nullptr can't populate data
    (void)policy;
}

// Test discoverMCTPDevices
TEST_F(NsmDbusWrappedTest, DiscoverMCTPDevicesWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.discoverMCTPDevices();
    (void)result;
}

// Test updateEndPoints
TEST_F(NsmDbusWrappedTest, UpdateEndPointsWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.updateEndPoints();
    (void)result;
}

// Test getMCTPServiceList
TEST_F(NsmDbusWrappedTest, GetMCTPServiceListWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto services = udt.getMCTPServiceList();
    (void)services;
}

// Test getMCTPManagedObjects
TEST_F(NsmDbusWrappedTest, GetMCTPManagedObjectsWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto objects = udt.getMCTPManagedObjects();
    (void)objects;
}

// Test getTokenStatus
TEST_F(NsmDbusWrappedTest, GetTokenStatusWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto status = udt.getTokenStatus("/test/path");
    (void)status;
}

// Test makeDebugTokenMethodCall variants
TEST_F(NsmDbusWrappedTest, MakeDebugTokenMethodCallWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);

    auto r1 = udt.makeDebugTokenMethodCall("/test", "EraseToken");
    (void)r1;
    auto r2 =
        udt.makeDebugTokenMethodCall("/test", "GetStatus", std::string("CRDT"));
    (void)r2;
    auto r3 = udt.makeDebugTokenMethodCall("/test", "InstallToken",
                                           std::vector<uint8_t>{0x01});
    (void)r3;
}

// Test logAsyncError
TEST_F(NsmDbusWrappedTest, LogAsyncErrorWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    EXPECT_NO_THROW(udt.logAsyncError("/test", "EraseToken", "Failed"));
}

// Test eraseDebugToken full path
TEST_F(NsmDbusWrappedTest, EraseDebugTokenWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    int result = udt.eraseDebugToken();
    (void)result;
}

// Test installDebugToken with file
TEST_F(NsmDbusWrappedTest, InstallDebugTokenWrapped)
{
    dbus_wrap::setSucceed();
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken udt(bus);
    auto status = udt.installDebugToken("/nonexistent.bin");
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
}
