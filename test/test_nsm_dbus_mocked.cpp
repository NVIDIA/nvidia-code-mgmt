/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Deep D-Bus success-path tests using pldm-style InSequence EXPECT_CALL
 * chains on SdBusMock. This enables reply.read() to deserialize data,
 * unlocking the loop bodies in NSM functions.
 */

#include <sdbusplus/test/sdbus_mock.hpp>

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

#include <deque>
#include <filesystem>
#include <fstream>

using ::testing::_;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrEq;

// Helper class with reusable mock expectations for D-Bus operations
class NsmMockedTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    UpdateDebugToken udt{bus};

    // Mock a new_method_call
    void expectNewMethodCall()
    {
        EXPECT_CALL(mock,
                    sd_bus_message_new_method_call(nullptr, _, _, _, _, _))
            .WillOnce(Return(0));
    }

    // Mock bus.call() returning success with nullptr reply
    void expectBusCall()
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
            .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                         sd_bus_message** reply) {
                *reply = nullptr;
                return 0;
            });
    }

    // Mock bus.call_noreply()
    void expectBusCallNoReply()
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, nullptr))
            .WillOnce(Return(0));
    }

    // Mock reading an empty array: enter_container(array) -> at_end=1 -> exit
    void expectReadEmptyArray()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
            .WillOnce(Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));
    }

    // Mock reading a string from a message
    void expectReadString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 's', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value;
                return 1;
            });
    }

    // Mock reading a variant containing a string
    void expectReadVariantString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'v', _))
            .WillOnce(Return(0));
        expectReadString(value);
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));
    }

    // Mock reading an object path
    void expectReadObjectPath(const char* path)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 'o', _))
            .WillOnce([path](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = path;
                return 1;
            });
    }

    // Mock appending a basic type (used in method.append())
    void expectAppendBasic()
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(nullptr, _, _))
            .WillRepeatedly(Return(0));
    }

    // Mock container operations for appending
    void expectAppendContainers()
    {
        EXPECT_CALL(mock, sd_bus_message_open_container(nullptr, _, _))
            .WillRepeatedly(Return(0));
        EXPECT_CALL(mock, sd_bus_message_close_container(nullptr))
            .WillRepeatedly(Return(0));
    }

    // Mock reading a GetSubTreeResponse with one endpoint path
    void expectReadGetSubTreeOneEndpoint(const char* endpointPath,
                                         const char* service)
    {
        // Enter outer array a{oa{sa{sv}}}
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
            .WillOnce(Return(0));

        // First entry exists
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(0));

        // Enter dict entry {oa{sa{sv}}}
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'e', _))
            .WillOnce(Return(0));

        // Read object path
        expectReadObjectPath(endpointPath);

        // Read inner array a{sa{sv}} - service map
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
            .WillOnce(Return(0));

        // First service entry
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(0));

        // Enter dict entry {sa{sv}}
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'e', _))
            .WillOnce(Return(0));

        // Read service name
        expectReadString(service);

        // Read interfaces array a{sv} - empty
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
            .WillOnce(Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));

        // Exit dict entry {sa{sv}}
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));

        // No more service entries
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(1));

        // Exit inner array a{sa{sv}}
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));

        // Exit dict entry {oa{sa{sv}}}
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));

        // No more outer entries
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(1));

        // Exit outer array
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));
    }
};

// ========================== enumerateNsmDebugTokenEndpoints ================

TEST_F(NsmMockedTest, EnumerateNsmEndpointsEmptyResponse)
{
    // NiceMock handles new_method_call, append, etc. by default.
    // We only need to mock bus.call() and the read sequence.

    // bus.call() returns success
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    // reply.read(objects) for empty GetSubTreeResponse: enter array, at_end=1
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    NSMEndpoints endpoints;
    int result = udt.enumerateNsmDebugTokenEndpoints(endpoints);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(endpoints.empty());
}

// ========================== nsmTokenErase with mocked empty response ======

TEST_F(NsmMockedTest, NsmTokenEraseEmptyEndpoints)
{
    // Mock bus.call() + empty array read for enumeration
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, 0);
}

// ========================== nsmTokenInstall with mocked empty response =====

TEST_F(NsmMockedTest, NsmTokenInstallEmptyEndpoints)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, 0);
}

// ========================== enumerateNsmEndpointsV2 empty ==================

TEST_F(NsmMockedTest, EnumerateNsmEndpointsV2Empty)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    NSMEndpoints endpoints;
    int result = udt.enumerateNsmDebugTokenEndpointsV2(endpoints);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(endpoints.empty());
}

// ========================== nsmTokenEraseV2/InstallV2 empty ================

TEST_F(NsmMockedTest, NsmTokenEraseV2EmptyEndpoints)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1); // empty -> error
}

// ========================== Enumerate with ONE endpoint ===================
// This test makes enumerateNsmDebugTokenEndpoints return one endpoint.
// The bus.call() returns 0, and reply.read() deserializes one path.

TEST_F(NsmMockedTest, EnumerateNsmEndpointsOneEndpoint)
{
    static const char* endpointPath = "/xyz/openbmc_project/NSM/gpu0";
    static const char* serviceName = "xyz.openbmc_project.NSM";

    // bus.call() for GetSubTree
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    // Read GetSubTreeResponse: a{oa{sa{sv}}}
    // Outer array
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0))  // outer array
        .WillOnce(Return(0))  // inner array a{sa{sv}}
        .WillOnce(Return(0)); // interfaces array a{sv} (empty)

    // at_end checks: first entry exists(0), inner array end(0),
    // interfaces end(1), no more services(1), no more outer(1)
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0))
        .WillOnce(Return(0))  // outer: has entry
        .WillOnce(Return(0))  // services: has entry
        .WillOnce(Return(1))  // interfaces: empty
        .WillOnce(Return(1))  // services: no more
        .WillOnce(Return(1)); // outer: no more

    // Enter dict entries
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'e', _))
        .WillOnce(Return(0))  // outer dict entry {oa{sa{sv}}}
        .WillOnce(Return(0)); // service dict entry {sa{sv}}

    // Read object path (as string or object_path) and service name
    // The first read_basic for 'o' or 's' returns the endpoint path,
    // subsequent ones return the service name
    static int readCount = 0;
    readCount = 0;
    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* output) {
            if (type == 'o' || type == 's')
            {
                if (readCount == 0)
                {
                    *static_cast<const char**>(output) = endpointPath;
                }
                else
                {
                    *static_cast<const char**>(output) = serviceName;
                }
                readCount++;
                return 1;
            }
            return 0;
        });

    // Exit containers (called multiple times)
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(Return(0));

    NSMEndpoints endpoints;
    int result = udt.enumerateNsmDebugTokenEndpoints(endpoints);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints[0], endpointPath);
}

// ========================== nsmTokenErase with one endpoint ===============
// Enumerate returns one endpoint, then getTokenStatus returns empty -> continue
TEST_F(NsmMockedTest, NsmTokenEraseOneEndpointEmptyStatus)
{
    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* svc = "xyz.openbmc_project.NSM";
    // Reset mock state counters

    // sd_bus_call is called multiple times:
    // 1. enumerateNsmDebugTokenEndpoints -> GetSubTree
    // 2. getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall ->
    // bus.call
    // 3. handleAsyncCall -> bus.call for status Get
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    // For the GetSubTree read: return one endpoint
    static int readBasicIdx = 0;
    readBasicIdx = 0;
    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* output) {
            if (type == 'o' || type == 's')
            {
                if (readBasicIdx == 0)
                    *static_cast<const char**>(output) = ep;
                else
                    *static_cast<const char**>(output) = svc;
                readBasicIdx++;
                return 1;
            }
            return 0;
        });

    // Container operations: first call sequence for GetSubTree
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, _, _))
        .WillRepeatedly(Return(0));

    static int atEndIdx = 0;
    atEndIdx = 0;
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            atEndIdx++;
            // For the GetSubTree read:
            // 1=has entry, 2=has service, 3=ifaces empty, 4=services done,
            // 5=outer done After that, reads for getTokenStatus which fail
            if (atEndIdx <= 1)
                return 0; // has entry
            if (atEndIdx == 2)
                return 0; // has service
            return 1;     // everything else ends
        });

    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(Return(0));

    int result = udt.nsmTokenErase();
    // getTokenStatus returns empty -> loop continues -> status stays 0
    (void)result;
}

TEST_F(NsmMockedTest, NsmTokenInstallV2EmptyEndpoints)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, 0)).WillOnce(Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));

    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

// ========================== LARGE BATCH: Loop body coverage ================
// These tests use the proven pattern: mock sd_bus_call + read_basic +
// container ops to exercise loop bodies in NSM functions.

// Helper: set up mocks that make enumeration return one endpoint,
// then all subsequent D-Bus calls succeed but return empty/default data.
// This covers the loop entry + catch/continue paths.
class NsmMockedWithEndpointTest : public NsmMockedTest
{
  protected:
    void SetUp() override
    {
        // All bus.call() succeed with nullptr reply
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
            .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t,
                               sd_bus_error*, sd_bus_message** reply) {
                if (reply)
                    *reply = nullptr;
                return 0;
            });

        // All container operations succeed
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, _, _))
            .WillRepeatedly(Return(0));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillRepeatedly(Return(0));

        // read_basic: first call returns endpoint path, rest return service
        static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
        static const char* svc = "xyz.openbmc_project.NSM";
        readIdx_ = 0;
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
            .WillRepeatedly([this](sd_bus_message*, char type, void* output) {
                if (type == 'o')
                {
                    // Object path reads - return endpoint or async path
                    *static_cast<const char**>(output) =
                        (readIdx_ == 0) ? ep : "/com/nvidia/nsmd/async/1";
                    readIdx_++;
                    return 1;
                }
                if (type == 's')
                {
                    // String reads - return service name or status
                    static const char* strings[] = {
                        svc,                               // service name
                        "com.nvidia.Async.Status.Success", // async status
                        "NoTokenApplied",                  // token status
                    };
                    int idx = readIdx_++;
                    *static_cast<const char**>(output) =
                        strings[idx < 3 ? idx : 2];
                    return 1;
                }
                if (type == 'b')
                {
                    *static_cast<int*>(output) = 1;
                    return 1;
                }
                return 0;
            });

        // verify_type always returns 1 (type matches)
        EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
            .WillRepeatedly(Return(1));

        // get_signature returns a valid signature for whatever is asked
        EXPECT_CALL(mock, sd_bus_message_get_signature(nullptr, _))
            .WillRepeatedly(Return("s"));

        // get_type returns method return (SD_BUS_MESSAGE_METHOD_RETURN = 2)
        EXPECT_CALL(mock, sd_bus_message_get_type(nullptr, _))
            .WillRepeatedly([](sd_bus_message*, uint8_t* type) {
                *type = 2; // SD_BUS_MESSAGE_METHOD_RETURN
                return 0;
            });

        // skip operations succeed
        EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
            .WillRepeatedly(Return(0));

        // at_end: first few return 0 (has data), then 1 (end)
        atEndIdx_ = 0;
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
            .WillRepeatedly([this](sd_bus_message*, int) {
                return (atEndIdx_++ < 2) ? 0 : 1;
            });
    }

    int readIdx_ = 0;
    int atEndIdx_ = 0;
};

// nsmTokenInstall with one endpoint - getTokenStatus fails -> loop continues
TEST_F(NsmMockedWithEndpointTest, NsmTokenInstallOneEndpointStatusFails)
{
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    // getTokenStatus fails -> loop continues for all endpoints
    (void)result;
}

// nsmTokenErase with one endpoint
TEST_F(NsmMockedWithEndpointTest, NsmTokenEraseOneEndpoint)
{
    int result = udt.nsmTokenErase();
    (void)result;
}

// nsmTokenEraseV2 with one endpoint
TEST_F(NsmMockedWithEndpointTest, NsmTokenEraseV2OneEndpoint)
{
    int result = udt.nsmTokenEraseV2();
    (void)result;
}

// nsmTokenInstallV2 with one endpoint
TEST_F(NsmMockedWithEndpointTest, NsmTokenInstallV2OneEndpoint)
{
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// handleAsyncCall with mocked bus
TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallMocked)
{
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    (void)result;
}

TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallInstallMocked)
{
    std::vector<uint8_t> token(100, 0x42);
    auto result = udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0",
                                      "InstallToken", token);
    (void)result;
}

TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallGetStatusMocked)
{
    auto result = udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0",
                                      "GetStatus", std::string("CRDT"));
    (void)result;
}

// handleAsyncCallInstallV2 with mocked bus
TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallInstallV2Mocked)
{
    int memfd = memfd_create("test", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(64, 0xAA);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);
        auto result = udt.handleAsyncCallInstallV2(
            "/xyz/openbmc_project/NSM/gpu0", memfd);
        (void)result;
        close(memfd);
    }
}

// handleAsyncCallEraseV2 with mocked bus
TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallEraseV2Mocked)
{
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    (void)result;
}

// getTokenStatus with mocked bus
TEST_F(NsmMockedWithEndpointTest, GetTokenStatusMocked)
{
    auto result = udt.getTokenStatus("/xyz/openbmc_project/NSM/gpu0");
    (void)result;
}

// makeDebugTokenMethodCall variants with mocked bus
TEST_F(NsmMockedWithEndpointTest, MakeDebugTokenMethodCallMocked)
{
    auto r1 = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                           "EraseToken");
    (void)r1;
    auto r2 = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                           "GetStatus", std::string("CRDT"));
    (void)r2;
    auto r3 = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                           "InstallToken",
                                           std::vector<uint8_t>{0x01, 0x02});
    (void)r3;
}

// logAsyncError with mocked bus
TEST_F(NsmMockedWithEndpointTest, LogAsyncErrorMocked)
{
    EXPECT_NO_THROW(udt.logAsyncError("/xyz/openbmc_project/NSM/gpu0",
                                      "EraseToken", "Failed"));
}

// getErasePolicy with mocked bus
TEST_F(NsmMockedWithEndpointTest, GetErasePolicyMocked)
{
    auto policy = udt.getErasePolicy();
    (void)policy;
}

// discoverMCTPDevices with mocked bus
TEST_F(NsmMockedWithEndpointTest, DiscoverMCTPDevicesMocked)
{
    int result = udt.discoverMCTPDevices();
    (void)result;
}

// getMCTPServiceList with mocked bus
TEST_F(NsmMockedWithEndpointTest, GetMCTPServiceListMocked)
{
    auto services = udt.getMCTPServiceList();
    (void)services;
}

// eraseDebugToken full path with mocked bus
TEST_F(NsmMockedWithEndpointTest, EraseDebugTokenMocked)
{
    int result = udt.eraseDebugToken();
    (void)result;
}

// nsmTokenInstall with pre-populated devices map
TEST_F(NsmMockedWithEndpointTest, NsmTokenInstallWithDevices)
{
    // Pre-populate the devices map so the loop body has matching serial
    udt.devices[42] = "0x0102030405060708";
    udt.deviceNameMap[42] = "GPU_ERoT_0";

    TokenMap tokens;
    tokens.emplace("0x0102030405060708", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// nsmTokenErase with pre-populated devices
TEST_F(NsmMockedWithEndpointTest, NsmTokenEraseWithDevices)
{
    udt.devices[42] = "0x0102030405060708";
    udt.deviceNameMap[42] = "GPU_ERoT_0";

    int result = udt.nsmTokenErase();
    (void)result;
}

// nsmTokenInstallV2 with token data
TEST_F(NsmMockedWithEndpointTest, NsmTokenInstallV2WithToken)
{
    TokenMap tokens;
    // Need serial >= 45 bytes for the header strip (line 363)
    tokens.emplace("serial_xyz", std::vector<uint8_t>(100, 0xAB));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// nsmTokenEraseV2
TEST_F(NsmMockedWithEndpointTest, NsmTokenEraseV2WithEndpoint)
{
    int result = udt.nsmTokenEraseV2();
    (void)result;
}

// handleAsyncCallInstallV2 with valid memfd
TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallInstallV2WithData)
{
    int memfd = memfd_create("token_data", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(256, 0xCC);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);
        auto result = udt.handleAsyncCallInstallV2(
            "/xyz/openbmc_project/NSM/gpu0", memfd);
        (void)result;
        close(memfd);
    }
}

// handleAsyncCallEraseV2
TEST_F(NsmMockedWithEndpointTest, HandleAsyncCallEraseV2WithPath)
{
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    (void)result;
}

// getAsyncValue with mocked bus
TEST_F(NsmMockedWithEndpointTest, GetAsyncValueMocked)
{
    try
    {
        auto result = udt.getAsyncValue("/com/nvidia/nsmd/async/1");
        (void)result;
    }
    catch (...)
    {
        // Expected - variant read may fail
    }
}

// logAsyncError with various statuses
TEST_F(NsmMockedWithEndpointTest, LogAsyncErrorVariousStatuses)
{
    EXPECT_NO_THROW(udt.logAsyncError("/test", "EraseToken", "Failed"));
    EXPECT_NO_THROW(udt.logAsyncError("/test", "InstallToken", "Error"));
    EXPECT_NO_THROW(udt.logAsyncError("/test", "DisableTokens", "Timeout"));
}

// createMessageRegistry variants
TEST_F(NsmMockedWithEndpointTest, CreateMessageRegistryVariants)
{
    EXPECT_NO_THROW(udt.createMessageRegistry("Update.1.0.UpdateSuccessful",
                                              "DebugToken", "1.0"));
    EXPECT_NO_THROW(udt.createMessageRegistry("Update.1.0.TransferFailed",
                                              "DebugToken", "1.0"));
}

// getErasePolicy - tested with dedicated mock setup
class GetErasePolicyMockedTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    UpdateDebugToken udt{bus};

    int callPhase = 0; // 0=GetSubTree, 1=GetProperty, 2+=done
    int readIdx = 0;
};

TEST_F(GetErasePolicyMockedTest, ReturnsManual)
{
    static const char* policyPath = "/com/nvidia/debug_token/policy";
    static const char* policyService = "com.nvidia.DebugToken";
    static const char* policyValue = "Manual";

    // sd_bus_call: first call for GetSubTree, second for GetProperty
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([this](sd_bus*, sd_bus_message*, uint64_t,
                               sd_bus_error*, sd_bus_message** reply) {
            *reply = nullptr;
            callPhase++;
            return 0;
        });

    // read_basic: returns appropriate data based on phase
    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([this](sd_bus_message*, char type, void* output) {
            if (type == 'o' || type == 's')
            {
                const char* val;
                if (callPhase <= 1)
                {
                    // Phase 1: GetSubTree - return path, then service
                    val = (readIdx == 0) ? policyPath : policyService;
                }
                else
                {
                    // Phase 2: GetProperty - return policy value
                    val = policyValue;
                }
                *static_cast<const char**>(output) = val;
                readIdx++;
                return 1;
            }
            return 0;
        });

    // Container operations
    EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, _, _))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(Return(0));

    // at_end: for GetSubTree array read
    int atEndCount = 0;
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([&atEndCount](sd_bus_message*, int) {
            return (atEndCount++ < 2) ? 0 : 1;
        });

    // verify_type for variant read
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    auto policy = udt.getErasePolicy();
    EXPECT_EQ(policy, "Manual");
}

// discoverMCTPDevices
TEST_F(NsmMockedWithEndpointTest, DiscoverMCTPDevicesPath)
{
    int result = udt.discoverMCTPDevices();
    (void)result;
}

// updateEndPoints
TEST_F(NsmMockedWithEndpointTest, UpdateEndPointsPath)
{
    int result = udt.updateEndPoints();
    (void)result;
}

// getMCTPServiceList
TEST_F(NsmMockedWithEndpointTest, GetMCTPServiceListPath)
{
    auto services = udt.getMCTPServiceList();
    (void)services;
}

// getMCTPManagedObjects
TEST_F(NsmMockedWithEndpointTest, GetMCTPManagedObjectsPath)
{
    auto objects = udt.getMCTPManagedObjects();
    (void)objects;
}

class MctpScriptedMockedTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    UpdateDebugToken udt{bus};

    struct BasicRead
    {
        char type;
        std::variant<std::string, uint8_t> value;
    };

    std::deque<BasicRead> basicReads;
    std::deque<int> atEndResults;
    std::deque<std::string> variantSignatures;
    std::vector<std::vector<uint8_t>> arrayReads;
    size_t arrayReadIndex = 0;

    void SetUp() override
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
            .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t,
                               sd_bus_error*, sd_bus_message** reply) {
                if (reply)
                {
                    *reply = nullptr;
                }
                return 0;
            });

        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, _, _))
            .WillRepeatedly(
                [this](sd_bus_message*, char type, const char* contents) {
                    if (type == SD_BUS_TYPE_VARIANT)
                    {
                        EXPECT_FALSE(variantSignatures.empty());
                        EXPECT_EQ(variantSignatures.front(),
                                  std::string(contents ? contents : ""));
                        variantSignatures.pop_front();
                    }
                    return 0;
                });

        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillRepeatedly(Return(0));

        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
            .WillRepeatedly([this](sd_bus_message*, int) {
                if (atEndResults.empty())
                {
                    return 1;
                }
                int value = atEndResults.front();
                atEndResults.pop_front();
                return value;
            });

        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
            .WillRepeatedly([this](sd_bus_message*, char type, void* output) {
                EXPECT_FALSE(basicReads.empty());
                auto next = basicReads.front();
                basicReads.pop_front();
                EXPECT_EQ(next.type, type);
                if (type == SD_BUS_TYPE_STRING ||
                    type == SD_BUS_TYPE_OBJECT_PATH)
                {
                    static thread_local std::string storage;
                    storage = std::get<std::string>(next.value);
                    *static_cast<const char**>(output) = storage.c_str();
                    return 1;
                }
                if (type == SD_BUS_TYPE_BYTE)
                {
                    *static_cast<uint8_t*>(output) =
                        std::get<uint8_t>(next.value);
                    return 1;
                }
                ADD_FAILURE() << "unexpected read_basic type " << type;
                return 0;
            });

        EXPECT_CALL(mock,
                    sd_bus_message_read_array(nullptr, SD_BUS_TYPE_BYTE, _, _))
            .WillRepeatedly(
                [this](sd_bus_message*, char, const void** ptr, size_t* size) {
                    EXPECT_LT(arrayReadIndex, arrayReads.size());
                    const auto& bytes = arrayReads[arrayReadIndex++];
                    *ptr = bytes.data();
                    *size = bytes.size();
                    return 1;
                });

        EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
            .WillRepeatedly(
                [this](sd_bus_message*, uint8_t type, const char* contents) {
                    if (type != SD_BUS_TYPE_VARIANT)
                    {
                        return 1;
                    }
                    EXPECT_FALSE(variantSignatures.empty());
                    return variantSignatures.front() ==
                                   std::string(contents ? contents : "")
                               ? 1
                               : 0;
                });

        EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
            .WillRepeatedly(Return(0));
    }

    void queueGetSubTreeOneService(const std::string& path,
                                   const std::string& service,
                                   const std::string& iface)
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 1, 1});
        basicReads.push_back({SD_BUS_TYPE_STRING, path});
        basicReads.push_back({SD_BUS_TYPE_STRING, service});
        basicReads.push_back({SD_BUS_TYPE_STRING, iface});
    }

    void queueGetSubTreeTwoServices(const std::string& path1,
                                    const std::string& service1,
                                    const std::string& iface1,
                                    const std::string& path2,
                                    const std::string& service2,
                                    const std::string& iface2)
    {
        atEndResults.insert(atEndResults.end(),
                            {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1});
        basicReads.push_back({SD_BUS_TYPE_STRING, path1});
        basicReads.push_back({SD_BUS_TYPE_STRING, service1});
        basicReads.push_back({SD_BUS_TYPE_STRING, iface1});
        basicReads.push_back({SD_BUS_TYPE_STRING, path2});
        basicReads.push_back({SD_BUS_TYPE_STRING, service2});
        basicReads.push_back({SD_BUS_TYPE_STRING, iface2});
    }

    void queueManagedObjectsOneValidMctpObject(
        const std::string& objectPath, const std::string& uuid, uint8_t eid,
        const std::string& medium, const std::string& binding,
        const std::string& connectivity,
        std::vector<uint8_t> supportedTypes = {mctpTypeSPDM, mctpTypeVDMIANA})
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 0, 0, 1, 0, 0, 1,
                                                 0, 0, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpEndpointEnableIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "Connectivity"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, connectivity});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "UUID"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, uuid});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpBindingIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "BindingType"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, binding});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "EID"});
        variantSignatures.push_back("y");
        basicReads.push_back({SD_BUS_TYPE_BYTE, eid});
        basicReads.push_back({SD_BUS_TYPE_STRING, "MediumType"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, medium});
        basicReads.push_back({SD_BUS_TYPE_STRING, "SupportedMessageTypes"});
        variantSignatures.push_back("ay");
        arrayReads.push_back(std::move(supportedTypes));
    }

    void queueManagedObjectsOnePldmObject(const std::string& objectPath,
                                          const std::string& uuid,
                                          const std::string& serial)
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "UUID"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, uuid});

        basicReads.push_back({SD_BUS_TYPE_STRING, pldmInventoryIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "SerialNumber"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, serial});
    }

    void queueManagedObjectsOnePldmObjectWithByteUuid(
        const std::string& objectPath, uint8_t uuid, const std::string& serial)
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "UUID"});
        variantSignatures.push_back("y");
        basicReads.push_back({SD_BUS_TYPE_BYTE, uuid});

        basicReads.push_back({SD_BUS_TYPE_STRING, pldmInventoryIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "SerialNumber"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, serial});
    }

    void queueManagedObjectsOneUuidOnlyObject(const std::string& objectPath,
                                              const std::string& uuid)
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "UUID"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, uuid});
    }

    void queueManagedObjectsOneEndpointOnlyObject(
        const std::string& objectPath, uint8_t eid, const std::string& medium,
        std::vector<uint8_t> supportedTypes = {mctpTypeSPDM, mctpTypeVDMIANA})
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "EID"});
        variantSignatures.push_back("y");
        basicReads.push_back({SD_BUS_TYPE_BYTE, eid});
        basicReads.push_back({SD_BUS_TYPE_STRING, "MediumType"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, medium});
        basicReads.push_back({SD_BUS_TYPE_STRING, "SupportedMessageTypes"});
        variantSignatures.push_back("ay");
        arrayReads.push_back(std::move(supportedTypes));
    }

    void queueManagedObjectsOneMctpObjectWithoutUuidProperty(
        const std::string& objectPath, uint8_t eid, const std::string& medium,
        const std::string& binding, const std::string& connectivity,
        std::vector<uint8_t> supportedTypes = {mctpTypeSPDM, mctpTypeVDMIANA})
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 0, 0, 1, 0, 0, 1,
                                                 0, 0, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpEndpointEnableIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "Connectivity"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, connectivity});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "Other"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, "ignored"});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpBindingIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "BindingType"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, binding});

        basicReads.push_back({SD_BUS_TYPE_STRING, mctpEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "EID"});
        variantSignatures.push_back("y");
        basicReads.push_back({SD_BUS_TYPE_BYTE, eid});
        basicReads.push_back({SD_BUS_TYPE_STRING, "MediumType"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, medium});
        basicReads.push_back({SD_BUS_TYPE_STRING, "SupportedMessageTypes"});
        variantSignatures.push_back("ay");
        arrayReads.push_back(std::move(supportedTypes));
    }

    void queueManagedObjectsOnePldmObjectWithoutSerialNumber(
        const std::string& objectPath, const std::string& uuid)
    {
        atEndResults.insert(atEndResults.end(), {0, 0, 0, 1, 0, 0, 1, 1, 1});

        basicReads.push_back({SD_BUS_TYPE_OBJECT_PATH, objectPath});

        basicReads.push_back({SD_BUS_TYPE_STRING, uuidEndpointIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "UUID"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, uuid});

        basicReads.push_back({SD_BUS_TYPE_STRING, pldmInventoryIntfName});
        basicReads.push_back({SD_BUS_TYPE_STRING, "Other"});
        variantSignatures.push_back("s");
        basicReads.push_back({SD_BUS_TYPE_STRING, "ignored"});
    }
};

TEST_F(MctpScriptedMockedTest, GetErasePolicyMultipleResultsReturnsEmpty)
{
    queueGetSubTreeTwoServices("/com/nvidia/debug_token/policy0",
                               "com.nvidia.DebugToken0", erasePolicyIntfName,
                               "/com/nvidia/debug_token/policy1",
                               "com.nvidia.DebugToken1", erasePolicyIntfName);

    EXPECT_TRUE(udt.getErasePolicy().empty());
}

TEST_F(MctpScriptedMockedTest, GetErasePolicyManualStripsNamespace)
{
    queueGetSubTreeOneService("/com/nvidia/debug_token/policy",
                              "com.nvidia.DebugToken", erasePolicyIntfName);
    variantSignatures.push_back("s");
    basicReads.push_back(
        {SD_BUS_TYPE_STRING, "com.nvidia.DebugToken.ErasePolicy.Manual"});

    EXPECT_EQ(udt.getErasePolicy(), "Manual");
}

TEST_F(MctpScriptedMockedTest, GetErasePolicyWrongPropertyTypeReturnsEmpty)
{
    queueGetSubTreeOneService("/com/nvidia/debug_token/policy",
                              "com.nvidia.DebugToken", erasePolicyIntfName);
    variantSignatures.push_back("y");
    basicReads.push_back({SD_BUS_TYPE_BYTE, static_cast<uint8_t>(1)});

    EXPECT_TRUE(udt.getErasePolicy().empty());
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSuccessPath)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_EQ(udt.mctpInfo.size(), 1u);
    ASSERT_TRUE(udt.mctpInfo.contains("gpu-uuid"));
    EXPECT_EQ(udt.mctpInfo.at("gpu-uuid").eid, 42);
    EXPECT_TRUE(udt.mctpInfo.at("gpu-uuid").enabled);
}

TEST_F(MctpScriptedMockedTest, GetMCTPServiceListTwoServices)
{
    queueGetSubTreeTwoServices(
        "/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
        mctpEndpointIntfName, "/xyz/openbmc_project/mctp/network/8",
        "xyz.openbmc_project.MCTP", mctpEndpointIntfName);

    auto services = udt.getMCTPServiceList();
    EXPECT_THAT(services, testing::ElementsAre("au.com.codeconstruct.MCTP1",
                                               "xyz.openbmc_project.MCTP"));
}

TEST_F(MctpScriptedMockedTest, GetMCTPManagedObjectsAggregatesServices)
{
    queueGetSubTreeTwoServices(
        "/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
        mctpEndpointIntfName, "/xyz/openbmc_project/mctp/network/8",
        "xyz.openbmc_project.MCTP", mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "uuid-a", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOneValidMctpObject(
        "/xyz/openbmc_project/mctp/network/8/55", "uuid-b", 55,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", "Available");

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_EQ(objects.size(), 2u);
    EXPECT_TRUE(objects.contains(sdbusplus::message::object_path{
        "/au/com/codeconstruct/mctp1/network/1/42"}));
    EXPECT_TRUE(objects.contains(sdbusplus::message::object_path{
        "/xyz/openbmc_project/mctp/network/8/55"}));
}

TEST_F(MctpScriptedMockedTest, GetMCTPManagedObjectsReturnsEmptyOnServiceError)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            return 0;
        })
        .WillOnce(Return(-EIO));

    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_TRUE(objects.empty());
}

TEST_F(MctpScriptedMockedTest,
       GetMCTPManagedObjectsKeepsFirstServiceObjectsWhenSecondFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            return 0;
        })
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            return 0;
        })
        .WillOnce(Return(-EIO));

    queueGetSubTreeTwoServices(
        "/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
        mctpEndpointIntfName, "/xyz/openbmc_project/mctp/network/8",
        "xyz.openbmc_project.MCTP", mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "uuid-a", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_EQ(objects.size(), 1u);
    EXPECT_TRUE(objects.contains(sdbusplus::message::object_path{
        "/au/com/codeconstruct/mctp1/network/1/42"}));
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsSuccessPath)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOnePldmObject("/xyz/openbmc_project/software/GPU_ERoT_0",
                                     "gpu-uuid", "SN123");

    EXPECT_EQ(udt.updateEndPoints(), 0);
    ASSERT_EQ(udt.devices.size(), 1u);
    EXPECT_EQ(udt.devices.at(42), "SN123");
    EXPECT_EQ(udt.deviceNameMap.at(42), "GPU_ERoT_0");
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsFailsWhenPldmLookupCallFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            return 0;
        })
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            return 0;
        })
        .WillOnce(Return(-EIO));

    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.updateEndPoints(), -1);
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsBuildsDeviceMapForBindingOnlyPath)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "binding-only-uuid", 42, "",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOnePldmObject("/xyz/openbmc_project/software/GPU_ERoT_0",
                                     "binding-only-uuid", "SN999");

    EXPECT_EQ(udt.updateEndPoints(), 0);
    ASSERT_EQ(udt.devices.size(), 1u);
    EXPECT_EQ(udt.devices.at(42), "SN999");
    EXPECT_EQ(udt.deviceNameMap.at(42), "GPU_ERoT_0");
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesPrefersHigherPriorityMedium)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/40", "dup-uuid", 40,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_EQ(udt.mctpInfo.at("dup-uuid").eid, 40);

    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/41", "dup-uuid", 41,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_EQ(udt.mctpInfo.size(), 1u);
    EXPECT_EQ(udt.mctpInfo.at("dup-uuid").eid, 41);
    EXPECT_EQ(udt.mctpInfo.at("dup-uuid").medium,
              "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
}

TEST_F(MctpScriptedMockedTest,
       DiscoverMCTPDevicesKeepsExistingHigherPriorityMedium)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/40", "dup-uuid", 40,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_EQ(udt.mctpInfo.at("dup-uuid").eid, 40);

    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/41", "dup-uuid", 41,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_EQ(udt.mctpInfo.size(), 1u);
    EXPECT_EQ(udt.mctpInfo.at("dup-uuid").eid, 40);
    EXPECT_EQ(udt.mctpInfo.at("dup-uuid").medium,
              "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSkipsUnavailableEndpoint)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "disabled-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Unavailable");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSkipsEndpointWithEmptyUuid)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSkipsUuidOnlyObject)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneUuidOnlyObject(
        "/au/com/codeconstruct/mctp1/network/1/uuid-only", "uuid-only");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSkipsEndpointOnlyObject)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneEndpointOnlyObject(
        "/au/com/codeconstruct/mctp1/network/1/endpoint-only", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest,
       DiscoverMCTPDevicesSkipsUuidInterfaceWithoutUuidProperty)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneMctpObjectWithoutUuidProperty(
        "/au/com/codeconstruct/mctp1/network/1/no-uuid-property", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest,
       DiscoverMCTPDevicesSkipsEndpointWithoutMediumAndBinding)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "empty-medium-binding", 42,
        "", "", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesAllowsBindingOnlyEndpoint)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "binding-only-uuid", 42, "",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_TRUE(udt.mctpInfo.contains("binding-only-uuid"));
    EXPECT_EQ(udt.mctpInfo.at("binding-only-uuid").binding,
              "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesAllowsMediumOnlyEndpoint)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "medium-only-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", "", "Available");

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    ASSERT_TRUE(udt.mctpInfo.contains("medium-only-uuid"));
    EXPECT_EQ(udt.mctpInfo.at("medium-only-uuid").medium,
              "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
}

TEST_F(MctpScriptedMockedTest, DiscoverMCTPDevicesSkipsUnsupportedEndpoint)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "unsupported-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available",
        {mctpTypeSPDM});

    EXPECT_EQ(udt.discoverMCTPDevices(), 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsSkipsUnknownUuid)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOnePldmObject("/xyz/openbmc_project/software/GPU_ERoT_0",
                                     "other-uuid", "SN123");

    EXPECT_EQ(udt.updateEndPoints(), 0);
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsSkipsPldmEntryWithoutSerialNumber)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOnePldmObjectWithoutSerialNumber(
        "/xyz/openbmc_project/software/GPU_ERoT_0", "gpu-uuid");

    EXPECT_EQ(udt.updateEndPoints(), 0);
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(MctpScriptedMockedTest, UpdateEndPointsCatchesPldmUuidTypeErrors)
{
    queueGetSubTreeOneService("/au/com/codeconstruct/mctp1/network/1",
                              "au.com.codeconstruct.MCTP1",
                              mctpEndpointIntfName);
    queueManagedObjectsOneValidMctpObject(
        "/au/com/codeconstruct/mctp1/network/1/42", "gpu-uuid", 42,
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", "Available");
    queueManagedObjectsOnePldmObjectWithByteUuid(
        "/xyz/openbmc_project/software/GPU_ERoT_0", 0x2a, "SN123");

    EXPECT_EQ(udt.updateEndPoints(), -1);
    EXPECT_TRUE(udt.devices.empty());
}

// installDebugToken with valid TLV file
TEST_F(NsmMockedWithEndpointTest, InstallDebugTokenWithFile)
{
    // Create valid TLV v2 token file
    std::string tmpPath = "/tmp/test_install_mocked.bin";
    {
        // Build minimal TLV structure
        debug_token::StructureHeader hdr{};
        std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
        hdr.versionMajor = htole16(2);
        hdr.versionMinor = htole16(0);
        // DeviceSerialNumber item
        debug_token::ItemHeader itemHdr;
        itemHdr.type = htole16(0x0003);
        itemHdr.size = htole16(8);
        uint8_t serial[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        size_t itemSize = sizeof(itemHdr) + 8;
        hdr.size = htole32(static_cast<uint32_t>(itemSize));

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        f.write(reinterpret_cast<const char*>(&itemHdr), sizeof(itemHdr));
        f.write(reinterpret_cast<const char*>(serial), 8);
    }
    auto status = udt.installDebugToken(tmpPath);
    (void)status;
    std::filesystem::remove(tmpPath);
}
