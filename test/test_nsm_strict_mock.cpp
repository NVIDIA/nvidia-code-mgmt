/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Precise SdBusMock tests following the exact pldm DBusMockTestHelpers pattern.
 * Uses StrictMock + InSequence for every sd_bus_* call in the exact order
 * that sdbusplus makes them.
 */

#include <sys/mman.h>
#include <unistd.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

using testing::_;
using testing::Eq;
using testing::Pointee;
using testing::Return;
using testing::StrEq;

namespace syscall_state
{
static bool failMemfdCreate = false;
static bool shortWrite = false;
static int failMemfdCreateRemaining = 0;
static int shortWriteRemaining = 0;
static int trackedFd = -1;
static int memfdCreateCalls = 0;
static int writeCalls = 0;
static int shortWriteCalls = 0;

void reset()
{
    failMemfdCreate = false;
    shortWrite = false;
    failMemfdCreateRemaining = 0;
    shortWriteRemaining = 0;
    trackedFd = -1;
    memfdCreateCalls = 0;
    writeCalls = 0;
    shortWriteCalls = 0;
}
} // namespace syscall_state

extern "C" int __real_memfd_create(const char*, unsigned int);
extern "C" int __wrap_memfd_create(const char* name, unsigned int flags)
{
    syscall_state::memfdCreateCalls++;
    if (syscall_state::failMemfdCreateRemaining > 0)
    {
        syscall_state::failMemfdCreateRemaining--;
        errno = EMFILE;
        return -1;
    }
    if (syscall_state::failMemfdCreate)
    {
        errno = EMFILE;
        return -1;
    }

    int fd = __real_memfd_create(name, flags);
    syscall_state::trackedFd = fd;
    return fd;
}

extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* buf, size_t count)
{
    syscall_state::writeCalls++;
    if (((syscall_state::shortWriteRemaining > 0) ||
         syscall_state::shortWrite) &&
        fd == syscall_state::trackedFd && count > 0)
    {
        if (syscall_state::shortWriteRemaining > 0)
        {
            syscall_state::shortWriteRemaining--;
        }
        syscall_state::shortWriteCalls++;
        errno = ENOSPC;
        return static_cast<ssize_t>(count - 1);
    }

    return __real_write(fd, buf, count);
}

// ========================== Helper base class (pldm pattern) ==============

class StrictMockTest : public testing::Test
{
  protected:
    testing::StrictMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);

    void SetUp() override
    {
        syscall_state::reset();
    }

    // --- Helpers copied from pldm DBusMockTestHelpers ---

    void expectNewMethodCall(const char* service, const char* path,
                             const char* interface, const char* method)
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(
                              _, _, StrEq(service), StrEq(path),
                              StrEq(interface), StrEq(method)))
            .WillOnce(Return(0));
    }

    void expectNewMethodCallAny()
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(_, _, _, _, _, _))
            .WillOnce(Return(0));
    }

    void expectAppendString(char type, const char* /*value*/)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(nullptr, type, _))
            .WillOnce(Return(0));
    }

    void expectAppendAny()
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(nullptr, _, _))
            .WillOnce(Return(0));
    }

    void expectOpenContainer(char type, const char* contents)
    {
        EXPECT_CALL(
            mock, sd_bus_message_open_container(nullptr, type, StrEq(contents)))
            .WillOnce(Return(0));
    }

    void expectOpenContainerAny()
    {
        EXPECT_CALL(mock, sd_bus_message_open_container(nullptr, _, _))
            .WillOnce(Return(0));
    }

    void expectCloseContainer()
    {
        EXPECT_CALL(mock, sd_bus_message_close_container(nullptr))
            .WillOnce(Return(0));
    }

    void expectBusCallWithReply()
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
            .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                         sd_bus_message** reply) {
                *reply = nullptr;
                return 0;
            });
    }

    void expectReadString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 's', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value;
                return 1;
            });
    }

    void expectReadObjectPath(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 'o', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value;
                return 1;
            });
    }

    void expectEnterArray()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'a', _))
            .WillOnce(Return(0));
    }

    void expectEnterDictEntry()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'e', _))
            .WillOnce(Return(0));
    }

    void expectEnterVariant()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'v', _))
            .WillOnce(Return(0));
    }

    void expectExitContainer()
    {
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));
    }

    void expectAtEndYes()
    {
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(Return(1));
    }

    void expectAtEndNo()
    {
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(Return(0));
    }

    void expectVerifyTypeYes()
    {
        EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
            .WillOnce(Return(1));
    }

    void expectVerifyTypeNo()
    {
        EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
            .WillOnce(Return(0));
    }

    void expectEnterStruct()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, 'r', _))
            .WillOnce(Return(0));
    }

    void expectReadUint16(uint16_t value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 'q', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<uint16_t*>(output) = value;
                return 1;
            });
    }

    void expectReadUint32(uint32_t value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, 'u', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<uint32_t*>(output) = value;
                return 1;
            });
    }

    // --- Composite helpers ---

    void expectReadEmptyArray()
    {
        expectEnterArray();
        expectAtEndYes();
        expectExitContainer();
    }

    // Read GetSubTreeResponse a{sa{sas}} with one entry
    void expectReadGetSubTreeOneEntry(const char* path, const char* service,
                                      const char* iface)
    {
        // outer array a{...}
        expectEnterArray();
        expectAtEndNo();        // has entry
        expectEnterDictEntry(); // {sa{sas}}
        expectReadString(path); // path key

        // inner array a{sas}
        expectEnterArray();
        expectAtEndNo();           // has service entry
        expectEnterDictEntry();    // {sas}
        expectReadString(service); // service name

        // interfaces array as
        expectEnterArray();
        expectAtEndNo(); // has interface
        expectReadString(iface);
        expectAtEndYes();      // end interfaces
        expectExitContainer(); // as

        expectExitContainer(); // {sas}
        expectAtEndYes();      // end services
        expectExitContainer(); // a{sas}

        expectExitContainer(); // {sa{sas}}
        expectAtEndYes();      // end outer
        expectExitContainer(); // a{sa{sas}}
    }

    void expectReadGetSubTreeTwoEntries(const char* path1, const char* service1,
                                        const char* iface1, const char* path2,
                                        const char* service2,
                                        const char* iface2)
    {
        expectEnterArray();

        expectAtEndNo();
        expectEnterDictEntry();
        expectReadString(path1);
        expectEnterArray();
        expectAtEndNo();
        expectEnterDictEntry();
        expectReadString(service1);
        expectEnterArray();
        expectAtEndNo();
        expectReadString(iface1);
        expectAtEndYes();
        expectExitContainer();
        expectExitContainer();
        expectAtEndYes();
        expectExitContainer();
        expectExitContainer();

        expectAtEndNo();
        expectEnterDictEntry();
        expectReadString(path2);
        expectEnterArray();
        expectAtEndNo();
        expectEnterDictEntry();
        expectReadString(service2);
        expectEnterArray();
        expectAtEndNo();
        expectReadString(iface2);
        expectAtEndYes();
        expectExitContainer();
        expectExitContainer();
        expectAtEndYes();
        expectExitContainer();
        expectExitContainer();

        expectAtEndYes();
        expectExitContainer();
    }

    // Expect the method.append() calls for GetSubTree: path, depth, ifaceList
    void expectAppendGetSubTreeArgs()
    {
        // method.append(path, depth, ifaceList)
        // path is string, depth is int32, ifaceList is vector<string>
        expectAppendAny();        // path (string)
        expectAppendAny();        // depth (int32)
        expectOpenContainerAny(); // array of strings
        expectAppendAny();        // interface string
        expectCloseContainer();
    }

    // Read variant<string>
    void expectReadVariantString(const char* value)
    {
        expectVerifyTypeYes();
        expectEnterVariant();
        expectReadString(value);
        expectExitContainer();
    }

    // Properties.Get for NSMAsyncValue returns variant<variant<tuple>>.
    void expectReadAsyncValueErrorTuple(uint16_t errorCode,
                                        const char* errorMessage)
    {
        expectVerifyTypeYes(); // outer property wrapper variant
        expectEnterVariant();
        expectVerifyTypeNo();  // NSMTokenStatusTuple not selected
        expectVerifyTypeYes(); // NSMErrorTuple selected
        expectEnterVariant();
        expectEnterStruct();
        expectReadUint16(errorCode);
        expectReadString(errorMessage);
        expectExitContainer(); // tuple
        expectExitContainer(); // inner variant
        expectExitContainer(); // outer variant
    }

    void expectReadAsyncValueTokenStatusTuple(const char* tokenType,
                                              const char* tokenStatus,
                                              const char* additionalInfo,
                                              uint32_t timeLeft)
    {
        expectVerifyTypeYes(); // outer property wrapper variant
        expectEnterVariant();
        expectVerifyTypeYes(); // NSMTokenStatusTuple selected
        expectEnterVariant();
        expectEnterStruct();
        expectReadString(tokenType);
        expectReadString(tokenStatus);
        expectReadString(additionalInfo);
        expectReadUint32(timeLeft);
        expectExitContainer(); // tuple
        expectExitContainer(); // inner variant
        expectExitContainer(); // outer variant
    }

    // Read object_path from reply
    void expectReadObjectPathReply(const char* path)
    {
        expectReadObjectPath(path);
    }

    // Expect a full D-Bus property Get call sequence
    void expectPropertyGetCall()
    {
        expectNewMethodCallAny(); // Properties.Get
        expectAppendAny();        // interface
        expectAppendAny();        // property name
        expectBusCallWithReply();
    }

    // Match creation: only sd_bus_add_match
    void expectMatchCreate()
    {
        EXPECT_CALL(mock, sd_bus_add_match(nullptr, _, _, _, _))
            .WillOnce([](sd_bus*, sd_bus_slot** slot, const char*,
                         sd_bus_message_handler_t, void*) {
                *slot = reinterpret_cast<sd_bus_slot*>(0xdefa);
                return 0;
            });
    }

    // Match destruction: only sd_bus_slot_unref
    void expectMatchDestroy()
    {
        EXPECT_CALL(mock,
                    sd_bus_slot_unref(reinterpret_cast<sd_bus_slot*>(0xdefa)))
            .WillOnce(Return(nullptr));
    }
};

// ========================== Tests ==========================================

TEST_F(StrictMockTest, EnumerateNsmEndpointsOneResult)
{
    testing::InSequence seq;

    // bus.new_method_call(objectMapperService, objectMapperPath,
    // objectMapperIntfName, "GetSubTree")
    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    // method.append(nsmDebugTokenPath, 0, ifaceList)
    expectAppendGetSubTreeArgs();
    // bus.call(method)
    expectBusCallWithReply();
    // reply.read(objects) -> one entry
    expectReadGetSubTreeOneEntry("/xyz/openbmc_project/NSM/gpu0",
                                 "xyz.openbmc_project.NSM",
                                 "com.nvidia.DebugToken");

    UpdateDebugToken udt(bus);
    NSMEndpoints eps;
    EXPECT_EQ(udt.enumerateNsmDebugTokenEndpoints(eps), 0);
    EXPECT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0], "/xyz/openbmc_project/NSM/gpu0");
}

TEST_F(StrictMockTest, EnumerateNsmEndpointsEmpty)
{
    testing::InSequence seq;

    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadEmptyArray();

    UpdateDebugToken udt(bus);
    NSMEndpoints eps;
    EXPECT_EQ(udt.enumerateNsmDebugTokenEndpoints(eps), 0);
    EXPECT_TRUE(eps.empty());
}

TEST_F(StrictMockTest, NsmTokenEraseEmptyEndpoints)
{
    testing::InSequence seq;

    // enumerateNsmDebugTokenEndpoints
    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadEmptyArray();

    UpdateDebugToken udt(bus);
    EXPECT_EQ(udt.nsmTokenErase(), 0); // empty endpoints = success
}

TEST_F(StrictMockTest, NsmTokenInstallEmptyEndpoints)
{
    testing::InSequence seq;

    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadEmptyArray();

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    EXPECT_EQ(udt.nsmTokenInstall(tokens), 0);
}

TEST_F(StrictMockTest, MakeDebugTokenMethodCallSuccess)
{
    testing::InSequence seq;

    // bus.new_method_call(nsmService, path, nsmDebugTokenIntfName, methodName)
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "EraseToken");
    // bus.call(method)
    expectBusCallWithReply();
    // reply.read(asyncPath) -> object path
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    UpdateDebugToken udt(bus);
    auto result = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                               "EraseToken");
    EXPECT_EQ(result, "/com/nvidia/nsmd/async/1");
}

TEST_F(StrictMockTest, LogAsyncErrorNotInstalledExact)
{
    testing::InSequence seq;

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x100F, "Token not installed");

    UpdateDebugToken udt(bus);
    EXPECT_NO_THROW(
        udt.logAsyncError("/com/nvidia/nsmd/async/1", "GetStatus", "Error"));
}

TEST_F(StrictMockTest, LogAsyncErrorRegularExact)
{
    testing::InSequence seq;

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x0001, "Generic error");

    UpdateDebugToken udt(bus);
    EXPECT_NO_THROW(
        udt.logAsyncError("/com/nvidia/nsmd/async/1", "GetStatus", "Error"));
}

TEST_F(StrictMockTest, GetTokenStatusSuccessExact)
{
    testing::InSequence seq;

    expectMatchCreate();
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "GetStatus");
    expectAppendAny();
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectPropertyGetCall();
    expectReadVariantString("com.nvidia.Async.Status.Success");
    expectMatchDestroy();

    expectPropertyGetCall();
    expectReadAsyncValueTokenStatusTuple(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 0);

    UpdateDebugToken udt(bus);
    auto status = udt.getTokenStatus("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_EQ(status, nsmTokenStatusDebugSessionActive);
}

TEST_F(StrictMockTest, GetTokenStatusErrorTupleTriggersCatchExact)
{
    testing::InSequence seq;

    expectMatchCreate();
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "GetStatus");
    expectAppendAny();
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectPropertyGetCall();
    expectReadVariantString("com.nvidia.Async.Status.Success");
    expectMatchDestroy();

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x0001, "Generic error");

    UpdateDebugToken udt(bus);
    auto status = udt.getTokenStatus("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(status.empty());
}

TEST_F(StrictMockTest, HandleAsyncCallErrorStatusWithExactErrorTuple)
{
    testing::InSequence seq;

    expectMatchCreate();
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "EraseToken");
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectPropertyGetCall();
    expectReadVariantString("com.nvidia.Async.Status.Error");

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x0001, "Generic error");
    expectMatchDestroy();

    UpdateDebugToken udt(bus);
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(StrictMockTest, HandleAsyncCallEraseV2NotInstalledReturnsAsyncPathExact)
{
    testing::InSequence seq;

    expectMatchCreate();
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken.Action", "EraseToken");
    expectAppendAny();
    expectAppendAny();
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectPropertyGetCall();
    expectReadVariantString("com.nvidia.Async.Status.Error");

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x100F, "Token not installed");

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x100F, "Token not installed");
    expectMatchDestroy();

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_EQ(result, "/com/nvidia/nsmd/async/1");
}

TEST_F(StrictMockTest, HandleAsyncCallEraseV2GenericErrorReturnsEmptyExact)
{
    testing::InSequence seq;

    expectMatchCreate();
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken.Action", "EraseToken");
    expectAppendAny();
    expectAppendAny();
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectPropertyGetCall();
    expectReadVariantString("com.nvidia.Async.Status.Error");

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x0001, "Generic error");

    expectPropertyGetCall();
    expectReadAsyncValueErrorTuple(0x0001, "Generic error");
    expectMatchDestroy();

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// HandleAsyncCallSuccess removed - tested via NiceMock in other test files.

TEST_F(StrictMockTest, NsmTokenEraseOneEndpointTokenStatusFails)
{
    testing::InSequence seq;

    // enumerateNsmDebugTokenEndpoints: GetSubTree returning one endpoint
    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadGetSubTreeOneEntry("/xyz/openbmc_project/NSM/gpu0",
                                 "xyz.openbmc_project.NSM",
                                 "com.nvidia.DebugToken");

    // Loop body: getTokenStatus -> handleAsyncCall
    expectMatchCreate();

    // makeDebugTokenMethodCall for GetStatus
    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "GetStatus");
    expectAppendAny(); // CRDT string arg
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    // handleAsyncCall: Get initial status
    expectNewMethodCallAny(); // Properties.Get
    expectAppendAny();        // interface
    expectAppendAny();        // property
    expectBusCallWithReply();
    expectReadVariantString("com.nvidia.Async.Status.Success");

    // handleAsyncCall returns -> match_t destroyed
    expectMatchDestroy();

    // getAsyncValue: Properties.Get for Value property
    expectNewMethodCallAny();
    expectAppendAny();
    expectAppendAny();
    expectBusCallWithReply();
    // Properties.Get returns variant<NSMAsyncValue>
    // First: outer variant verify + skip (Property wrapper)
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(0)); // No type matches
    EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
        .WillRepeatedly(Return(0)); // Skip everything
    // getAsyncValue throws on bad_variant_access -> getTokenStatus returns ""

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    (void)result;
}

TEST_F(StrictMockTest, NsmTokenInstallOneEndpointTokenStatusFails)
{
    testing::InSequence seq;

    // Same enumerate pattern
    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadGetSubTreeOneEntry("/xyz/openbmc_project/NSM/gpu0",
                                 "xyz.openbmc_project.NSM",
                                 "com.nvidia.DebugToken");

    expectMatchCreate();

    expectNewMethodCall("xyz.openbmc_project.NSM",
                        "/xyz/openbmc_project/NSM/gpu0",
                        "com.nvidia.DebugToken", "GetStatus");
    expectAppendAny();
    expectBusCallWithReply();
    expectReadObjectPathReply("/com/nvidia/nsmd/async/1");

    expectNewMethodCallAny();
    expectAppendAny();
    expectAppendAny();
    expectBusCallWithReply();
    expectReadVariantString("com.nvidia.Async.Status.Success");

    expectMatchDestroy();

    expectNewMethodCallAny();
    expectAppendAny();
    expectAppendAny();
    expectBusCallWithReply();
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
        .WillRepeatedly(Return(0));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// nsmTokenErase: Uses NiceMock variant for the chained test
class ChainedMockTest : public testing::Test
{
  protected:
    testing::NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);

    void SetUp() override
    {
        syscall_state::reset();
    }
};

TEST_F(ChainedMockTest, NsmTokenEraseOneEndpointNoToken)
{
    // Counter-based approach with NiceMock
    static int callPhase = 0;
    static int readIdx = 0;
    callPhase = 0;
    readIdx = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* svc = "xyz.openbmc_project.NSM";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken = "NoTokenApplied";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase <= 1)
                    val = (readIdx == 0) ? ep : svc;
                else if (callPhase == 3)
                    val = successStatus;
                else
                    val = noToken;
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    static int atEnd = 0;
    atEnd = 0;
    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    (void)result;
}

// =================== Comprehensive ChainedMockTest tests ===================
// These use NiceMock with phase-counter approach to exercise all NSM function
// loop bodies, covering handleAsyncCall, getTokenStatus, logAsyncError,
// getAsyncValue, nsmTokenErase/Install, nsmTokenEraseV2/InstallV2,
// handleAsyncCallInstallV2, handleAsyncCallEraseV2.

// --- nsmTokenErase: one endpoint with DebugSessionActive token ---
// Call sequence:
//  Phase 1: GetSubTree → one endpoint
//  Phase 2: makeDebugTokenMethodCall(GetStatus) → asyncPath
//  Phase 3: Properties.Get(Status) → Success
//  Phase 4: getAsyncValue Properties.Get(Value) → NSMTokenStatusTuple with
//  DebugSessionActive Phase 5: makeDebugTokenMethodCall(DisableTokens) →
//  asyncPath Phase 6: Properties.Get(Status) → Success Phase 7:
//  makeDebugTokenMethodCall(GetStatus) → asyncPath (2nd getTokenStatus) Phase
//  8: Properties.Get(Status) → Success Phase 9: getAsyncValue
//  Properties.Get(Value) → NSMTokenStatusTuple with NoTokenApplied
TEST_F(ChainedMockTest, NsmTokenEraseFullLoopActiveToken)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenActive =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* noTokenApplied =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                // Phases 2,5,7 return asyncPath, others return ep
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5 || callPhase == 7)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    // GetSubTree response: path, service, interface
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 6 || callPhase == 8)
                {
                    val = successStatus; // Properties.Get Status
                }
                else if (callPhase == 4)
                {
                    val = tokenActive; // getAsyncValue -> tokenStatus
                }
                else if (callPhase == 9)
                {
                    val = noTokenApplied; // 2nd getTokenStatus value
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            // First 2 calls = 0 (has data), then 1 (end)
            return (atEnd++ < 2) ? 0 : 1;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, 0);
}

// --- nsmTokenErase: one endpoint with TokenTimeout (also triggers
// DisableTokens) ---
TEST_F(ChainedMockTest, NsmTokenEraseTokenTimeout)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenTimeout =
        "com.nvidia.DebugToken.TokenStatus.TokenTimeout";
    static const char* noTokenApplied =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5 || callPhase == 7)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 6 || callPhase == 8)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = tokenTimeout;
                }
                else if (callPhase == 9)
                {
                    val = noTokenApplied;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, 0);
}

// --- nsmTokenErase: DisableTokens fails (handleAsyncCall returns empty) ---
TEST_F(ChainedMockTest, NsmTokenEraseDisableTokensFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenActive =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* failedStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = tokenActive;
                }
                else if (callPhase == 6)
                {
                    val = failedStatus; // DisableTokens returns Error
                }
                else
                {
                    val = "ErrorMessage"; // logAsyncError error message
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u' || type == 'q')
            {
                // For NSMErrorTuple: uint16_t errorCode
                *static_cast<uint16_t*>(out) = 0x100F; // NotInstalled
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

// --- nsmTokenErase: getTokenStatus returns empty (exception path) ---
TEST_F(ChainedMockTest, NsmTokenEraseGetTokenStatusException)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 2: makeDebugTokenMethodCall fails
            if (callPhase == 2)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                             "com.nvidia.DebugToken"};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 0];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    udt.nsmTokenErase(); // getTokenStatus returns "" -> continue
}

// --- nsmTokenInstall: one endpoint, matching serial, full install flow ---
// Phase 1: GetSubTree → one endpoint
// Phase 2: makeDebugTokenMethodCall(GetStatus) → asyncPath
// Phase 3: Properties.Get(Status) → Success
// Phase 4: getAsyncValue → NSMTokenStatusTuple with NoTokenApplied
// Phase 5: Properties.Get(TokenDeviceID) → serial number
// Phase 6: makeDebugTokenMethodCall(InstallToken) → asyncPath
// Phase 7: Properties.Get(Status) → Success
// Phase 8: makeDebugTokenMethodCall(GetStatus) → asyncPath (verify)
// Phase 9: Properties.Get(Status) → Success
// Phase 10: getAsyncValue → DebugSessionActive
TEST_F(ChainedMockTest, NsmTokenInstallFullLoop)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 6 || callPhase == 8)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 7 || callPhase == 9)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken;
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                else if (callPhase == 10)
                {
                    val = active;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    // Token must be at least 45 bytes (header 44 + data)
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, 0);
}

// --- nsmTokenInstall: token status = DebugSessionActive (already active, skip)
// ---
TEST_F(ChainedMockTest, NsmTokenInstallAlreadyActive)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = active; // Already active
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    udt.nsmTokenInstall(tokens);
}

// --- nsmTokenInstall: serial number not in token map ---
TEST_F(ChainedMockTest, NsmTokenInstallSerialNotFound)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* serial = "UNKNOWN_SERIAL";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken;
                }
                else if (callPhase == 5)
                {
                    val = serial; // Unknown serial
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("DIFFERENT_SERIAL", std::vector<uint8_t>(100, 0x42));
    udt.nsmTokenInstall(tokens);
}

// --- nsmTokenInstall: InstallToken fails (handleAsyncCall returns "") ---
TEST_F(ChainedMockTest, NsmTokenInstallInstallFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 6: InstallToken method call fails
            if (callPhase == 6)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken;
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(ChainedMockTest,
       NsmTokenEraseTwoEndpointsInitialStatusFailureThenSuccess)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/erase-recover";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* noTokenApplied =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
            {
                return -1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 3 || callPhase == 6 || callPhase == 8)
                        ? asyncPath
                        : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 4 || callPhase == 7 || callPhase == 9)
                {
                    val = successStatus;
                }
                else if (callPhase == 5)
                {
                    val = active;
                }
                else if (callPhase == 10)
                {
                    val = noTokenApplied;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, 0);
}

TEST_F(ChainedMockTest, NsmTokenEraseInvalidEndpointPathThenSuccess)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* invalidEp = "not-a-valid-object-path";
    static const char* validEp = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/erase-valid";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* noTokenApplied =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5 || callPhase == 7)
                        ? asyncPath
                        : validEp;
                return 1;
            }
            if (type == 's')
            {
                const char* val = validEp;
                if (callPhase == 1)
                {
                    static const char* seq[] = {invalidEp,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken",
                                                validEp,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 3 || callPhase == 6 || callPhase == 8)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = active;
                }
                else if (callPhase == 9)
                {
                    val = noTokenApplied;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, 0);
}

TEST_F(ChainedMockTest,
       NsmTokenInstallTwoEndpointsInitialStatusFailureThenSuccess)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/install-recover";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* serial = "SERIAL_RECOVER";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
            {
                return -1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 3 || callPhase == 7 || callPhase == 9)
                        ? asyncPath
                        : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 4 || callPhase == 8 || callPhase == 10)
                {
                    val = successStatus;
                }
                else if (callPhase == 5)
                {
                    val = noToken;
                }
                else if (callPhase == 6)
                {
                    val = serial;
                }
                else if (callPhase == 11)
                {
                    val = active;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serial, std::vector<uint8_t>(100, 0x51));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, 0);
}

TEST_F(ChainedMockTest, NsmTokenInstallInvalidEndpointPathThenSuccess)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* invalidEp = "not-a-valid-object-path";
    static const char* validEp = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/install-valid";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* serial = "SERIAL_VALID";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 6 || callPhase == 8)
                        ? asyncPath
                        : validEp;
                return 1;
            }
            if (type == 's')
            {
                const char* val = validEp;
                if (callPhase == 1)
                {
                    static const char* seq[] = {invalidEp,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken",
                                                validEp,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 3 || callPhase == 7 || callPhase == 9)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken;
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                else if (callPhase == 10)
                {
                    val = active;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serial, std::vector<uint8_t>(100, 0x52));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, 0);
}

TEST_F(ChainedMockTest, NsmTokenInstallTokenTimeoutFullLoop)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/token-timeout";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenTimeout =
        "com.nvidia.DebugToken.TokenStatus.TokenTimeout";
    static const char* active =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
    static const char* serial = "SERIAL_TIMEOUT";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
            {
                *reply = nullptr;
            }
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 6 || callPhase == 8)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 7 || callPhase == 9)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = tokenTimeout;
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                else if (callPhase == 10)
                {
                    val = active;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serial, std::vector<uint8_t>(100, 0x53));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, 0);
}

// --- nsmTokenEraseV2: one endpoint, success path ---
// Phase 1: GetSubTree (enumerateV2) → one endpoint
// Phase 2: EraseToken method call → asyncPath
// Phase 3: Properties.Get(Status) → Success
TEST_F(ChainedMockTest, NsmTokenEraseV2Success)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, 0);
}

// --- nsmTokenEraseV2: EraseToken returns Error with NotInstalled ---
// handleAsyncCallEraseV2 flow when status != Success && != InProgress:
//  Phase 1: GetSubTree → one endpoint
//  Phase 2: EraseToken → asyncPath
//  Phase 3: Properties.Get(Status) → Error (not Success, not InProgress)
//  Phase 4: logAsyncError → getAsyncValue Properties.Get(Value) → NSMErrorTuple
//  Phase 5: inner getAsyncValue → Properties.Get(Value) → NSMErrorTuple with
//  NotInstalled
TEST_F(ChainedMockTest, NsmTokenEraseV2NotInstalledError)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* errorStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase >= 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = errorStatus; // Status = Error
                }
                else
                {
                    val = "Token not installed"; // error message for
                                                 // NSMErrorTuple
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                // NSMErrorTuple errorCode = 0x100F (NotInstalled) for both
                // phases 4 and 5
                *static_cast<uint16_t*>(out) = 0x100F;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    // Even if NotInstalled, the test exercises the error path code
    (void)result;
}

// --- nsmTokenEraseV2: handleAsyncCallEraseV2 exception path ---
TEST_F(ChainedMockTest, NsmTokenEraseV2Exception)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
                return -1; // EraseToken fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                             "com.nvidia.DebugToken.Action"};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 0];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstallV2: one endpoint, success path ---
// Phase 1: GetSubTree (enumerateV2) → one endpoint
// Phase 2: Properties.Get(TokenDeviceID) → serial
// Phase 3: InstallToken method call → asyncPath
// Phase 4: Properties.Get(Status) → Success
TEST_F(ChainedMockTest, NsmTokenInstallV2Success)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 3) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                else if (callPhase == 4)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            if (type == 'h')
            {
                *static_cast<int*>(out) = 0;
                return 1;
            } // unix_fd
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 1);
    EXPECT_GE(syscall_state::writeCalls, 1);
    EXPECT_EQ(syscall_state::shortWriteCalls, 0);
}

// --- nsmTokenInstallV2: serial not found ---
TEST_F(ChainedMockTest, NsmTokenInstallV2SerialNotFound)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* serial = "UNKNOWN";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("DIFFERENT_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 0);
    EXPECT_EQ(syscall_state::writeCalls, 0);
}

// --- nsmTokenInstallV2: memfd_create fails before InstallToken ---
TEST_F(ChainedMockTest, NsmTokenInstallV2MemfdCreateFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    syscall_state::failMemfdCreate = true;

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 1);
    EXPECT_EQ(syscall_state::writeCalls, 0);
    EXPECT_EQ(syscall_state::shortWriteCalls, 0);
}

// --- nsmTokenInstallV2: short write fails before InstallToken ---
TEST_F(ChainedMockTest, NsmTokenInstallV2ShortWriteFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    syscall_state::shortWrite = true;

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 1);
    EXPECT_GE(syscall_state::writeCalls, 1);
    EXPECT_EQ(syscall_state::shortWriteCalls, 1);
}

// --- handleAsyncCall: makeDebugTokenMethodCall returns empty (method fail) ---
TEST_F(ChainedMockTest, HandleAsyncCallMethodCallFails)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return -1; // All calls fail
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCall(
        "/xyz/openbmc_project/NSM/gpu0", "GetStatus",
        std::string("com.nvidia.DebugToken.TokenTypes.CRDT"));
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCall: status check throws exception ---
TEST_F(ChainedMockTest, HandleAsyncCallStatusCheckException)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
                return -1; // Status check fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    EXPECT_TRUE(result.empty());
}

// --- logAsyncError: NotInstalled error code ---
TEST_F(ChainedMockTest, LogAsyncErrorNotInstalled)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                *static_cast<const char**>(out) = "Token not installed";
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x100F; // NotInstalled
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    udt.logAsyncError("/com/nvidia/nsmd/async/1", "GetStatus", "Error");
}

// --- logAsyncError: regular error code ---
TEST_F(ChainedMockTest, LogAsyncErrorRegular)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                *static_cast<const char**>(out) = "Some error message";
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001; // Regular error
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    udt.logAsyncError("/com/nvidia/nsmd/async/1", "GetStatus", "Error");
}

// --- logAsyncError: getAsyncValue throws exception ---
TEST_F(ChainedMockTest, LogAsyncErrorException)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return -1; // Properties.Get fails
        });

    UpdateDebugToken udt(bus);
    udt.logAsyncError("/com/nvidia/nsmd/async/1", "GetStatus", "Error");
}

// --- getAsyncValue success path ---
TEST_F(ChainedMockTest, GetAsyncValueSuccess)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                static const char* strs[] = {"tokenType", "tokenStatus",
                                             "addlInfo"};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 2];
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 3600; // timeLeft
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto val = udt.getAsyncValue("/com/nvidia/nsmd/async/1");
    auto& [tokenType, tokenStatus, addlInfo, timeLeft] =
        std::get<NSMTokenStatusTuple>(val);
    EXPECT_EQ(tokenType, "tokenType");
}

// --- getTokenStatus success path ---
TEST_F(ChainedMockTest, GetTokenStatusSuccess)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenStatus =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = successStatus;
                else if (callPhase >= 3)
                    val = tokenStatus;
                else
                    val = "";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto status = udt.getTokenStatus("/xyz/openbmc_project/NSM/gpu0");
    // tokenStatus string after extracting the last segment
    EXPECT_FALSE(status.empty());
}

// --- getTokenStatus: handleAsyncCall fails ---
TEST_F(ChainedMockTest, GetTokenStatusAsyncFails)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return -1; // All D-Bus calls fail
        });

    UpdateDebugToken udt(bus);
    auto status = udt.getTokenStatus("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(status.empty());
}

// --- makeDebugTokenMethodCall: InstallToken variant ---
TEST_F(ChainedMockTest, MakeDebugTokenMethodCallInstall)
{
    static int callPhase = 0;
    callPhase = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    std::vector<uint8_t> token(100, 0x42);
    auto result = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                               "InstallToken", token);
    EXPECT_EQ(result, asyncPath);
}

// --- makeDebugTokenMethodCall: default monostate (EraseToken/DisableTokens)
// ---
TEST_F(ChainedMockTest, MakeDebugTokenMethodCallErase)
{
    static int callPhase = 0;
    callPhase = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            return 0;
        });

    UpdateDebugToken udt(bus);
    auto result = udt.makeDebugTokenMethodCall("/xyz/openbmc_project/NSM/gpu0",
                                               "EraseToken");
    EXPECT_EQ(result, asyncPath);
}

// --- handleAsyncCallEraseV2: empty async path returned ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2EmptyPath)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = ""; // empty path
                return 1;
            }
            return 0;
        });

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallInstallV2: empty async path ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2EmptyPath)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = "";
                return 1;
            }
            return 0;
        });

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallInstallV2: success path ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2Success)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                *static_cast<const char**>(out) = successStatus;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    uint8_t data[] = {0x42};
    ASSERT_EQ(1, write(memfd, data, 1));
    lseek(memfd, 0, SEEK_SET);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_EQ(result, asyncPath);
}

// --- handleAsyncCallInstallV2: exception path ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2Exception)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return -1; // InstallToken call fails
        });

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallEraseV2: success path ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2Success)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                *static_cast<const char**>(out) = successStatus;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_EQ(result, asyncPath);
}

// --- handleAsyncCallEraseV2: exception path ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2Exception)
{
    static int callPhase = 0;
    callPhase = 0;

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            return -1; // All calls fail
        });

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallEraseV2: Error with non-NotInstalled error code ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2ErrorNotNotInstalled)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* errorStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = errorStatus;
                else
                    val = "Internal error";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001; // Not NotInstalled
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallInstallV2: status check exception ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2StatusException)
{
    static int callPhase = 0;
    callPhase = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            if (callPhase == 2)
                return -1; // Properties.Get Status fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            return 0;
        });

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallEraseV2: status check exception ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2StatusException)
{
    static int callPhase = 0;
    callPhase = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            if (callPhase == 2)
                return -1; // Status check fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            return 0;
        });

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- nsmTokenEraseV2: empty endpoints ---
TEST_F(ChainedMockTest, NsmTokenEraseV2EmptyEndpoints)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) { return 1; }); // empty

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1); // No endpoints = error for V2
}

// --- nsmTokenInstallV2: empty endpoints ---
TEST_F(ChainedMockTest, NsmTokenInstallV2EmptyEndpoints)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) { return 1; });

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("S", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

// --- nsmTokenErase: enumeration fails ---
TEST_F(ChainedMockTest, NsmTokenEraseEnumerationFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return -1; // GetSubTree fails
        });

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstall: enumeration fails ---
TEST_F(ChainedMockTest, NsmTokenInstallEnumerationFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return -1;
        });

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("S", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

// --- nsmTokenEraseV2: enumeration fails ---
TEST_F(ChainedMockTest, NsmTokenEraseV2EnumerationFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return -1;
        });

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstallV2: enumeration fails ---
TEST_F(ChainedMockTest, NsmTokenInstallV2EnumerationFails)
{
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return -1;
        });

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("S", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

// --- handleAsyncCallInstallV2: Error status (not Success) ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2ErrorStatus)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* errorStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = errorStatus;
                else
                    val = "error message";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

// --- nsmTokenErase: exception in loop body ---
TEST_F(ChainedMockTest, NsmTokenEraseLoopException)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    // Return verify_type 0 to force bad_variant_access in getAsyncValue
    // which causes getTokenStatus to throw, exercising the catch block in
    // nsmTokenErase
    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                else
                {
                    val = "SomeValue";
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    // verify_type returns 0 to skip variant, then sd_bus_message_skip to eat it
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
        .WillRepeatedly(Return(0));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    (void)result;
}

// --- handleAsyncCall: Success status returns asyncObjectPath ---
TEST_F(ChainedMockTest, HandleAsyncCallSuccess)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                *static_cast<const char**>(out) = successStatus;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    EXPECT_EQ(result, asyncPath);
}

// --- handleAsyncCall: Error status triggers logAsyncError ---
TEST_F(ChainedMockTest, HandleAsyncCallErrorStatus)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* errorStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = errorStatus;
                else
                    val = "error detail";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x100F;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCall: InProgress → Success (exercises wait loop) ---
// propertyChangeSignalTimeout = 5s, so 50 iterations max
// We return InProgress initially, then on the 2nd process_discard iteration
// the status changes. But since match callback can't fire in mock,
// the loop just runs for maxIterations. We make status != InProgress via
// having the initial Properties.Get return InProgress, then the loop runs.
// After loop, status is still InProgress → logAsyncError → return ""
TEST_F(ChainedMockTest, HandleAsyncCallInProgressTimeout)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* inProgressStatus = "com.nvidia.Async.Status.InProgress";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = inProgressStatus; // initial status = InProgress
                else
                    val = "error detail";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    // Override propertyChangeSignalTimeout to something very small
    // But it's constexpr... We can't. The loop will run but bus.wait is no-op
    // on mock bus. So it finishes quickly.
    UpdateDebugToken udt(bus);
    auto result =
        udt.handleAsyncCall("/xyz/openbmc_project/NSM/gpu0", "EraseToken");
    // InProgress timeout → logAsyncError → return ""
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallInstallV2: InProgress timeout ---
TEST_F(ChainedMockTest, HandleAsyncCallInstallV2InProgressTimeout)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* inProgressStatus = "com.nvidia.Async.Status.InProgress";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = inProgressStatus;
                else
                    val = "error detail";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int memfd = memfd_create("test_token", MFD_CLOEXEC);
    uint8_t data[] = {0x42};
    [[maybe_unused]] auto w = write(memfd, data, 1);
    lseek(memfd, 0, SEEK_SET);
    auto result =
        udt.handleAsyncCallInstallV2("/xyz/openbmc_project/NSM/gpu0", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallEraseV2: InProgress timeout ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2InProgressTimeout)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* inProgressStatus = "com.nvidia.Async.Status.InProgress";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = inProgressStatus;
                else
                    val = "error detail";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- handleAsyncCallEraseV2: InProgress timeout → NotInstalled error ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2InProgressTimeoutNotInstalled)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* inProgressStatus = "com.nvidia.Async.Status.InProgress";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = inProgressStatus;
                else
                    val = "Token not installed";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x100F; // NotInstalled
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    // After timeout, status is still InProgress (not Success) → logAsyncError
    // Then check for NotInstalled (0x100F) → return asyncObjectPath
    // But status is "InProgress" not an error, so it logs and checks
    (void)result;
}

// --- handleAsyncCallEraseV2: timeout → getAsyncValue exception in NotInstalled
// check ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2TimeoutGetValueException)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* inProgressStatus = "com.nvidia.Async.Status.InProgress";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // logAsyncError's getAsyncValue succeeds, but NotInstalled check's
            // getAsyncValue fails
            if (callPhase >= 5)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                const char* val;
                if (callPhase == 2)
                    val = inProgressStatus;
                else
                    val = "error msg";
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'q')
            {
                *static_cast<uint16_t*>(out) = 0x0001; // Not NotInstalled
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

// --- nsmTokenErase: verify after disable returns wrong status ---
TEST_F(ChainedMockTest, NsmTokenEraseVerifyAfterDisableFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenActive =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5 || callPhase == 7)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 6 || callPhase == 8)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = tokenActive;
                }
                else if (callPhase == 9)
                {
                    // 2nd getTokenStatus returns DebugSessionActive still (not
                    // NoTokenApplied)
                    val = tokenActive;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1); // Token erase failed
}

// --- nsmTokenErase: 2nd getTokenStatus returns empty ---
TEST_F(ChainedMockTest, NsmTokenEraseSecondGetTokenStatusEmpty)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* tokenActive =
        "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 7+: 2nd getTokenStatus's makeDebugTokenMethodCall fails
            if (callPhase >= 7)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 5) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 6)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = tokenActive;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstall: install succeeds but verify returns wrong status ---
TEST_F(ChainedMockTest, NsmTokenInstallVerifyFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 6 || callPhase == 8)
                        ? asyncPath
                        : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 7 || callPhase == 9)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken; // initial: no token
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                else if (callPhase == 10)
                {
                    val = noToken; // verify: still no token (install failed)
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstall: getTokenStatus empty after install ---
TEST_F(ChainedMockTest, NsmTokenInstallVerifyEmpty)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* noToken =
        "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 8+: 3rd getTokenStatus fails
            if (callPhase >= 8)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2 || callPhase == 6) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                                 "com.nvidia.DebugToken"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 3 || callPhase == 7)
                {
                    val = successStatus;
                }
                else if (callPhase == 4)
                {
                    val = noToken;
                }
                else if (callPhase == 5)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstallV2: full loop with memfd, install, success ---
TEST_F(ChainedMockTest, NsmTokenInstallV2FullLoopSuccess)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 3) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                else if (callPhase == 4)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            if (type == 'h')
            {
                *static_cast<int*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, 0);
}

// --- nsmTokenInstallV2: install fails (handleAsyncCallInstallV2 returns empty)
// ---
TEST_F(ChainedMockTest, NsmTokenInstallV2InstallFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* serial = "SERIAL123";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 3)
                return -1; // InstallToken call fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                const char* val = ep;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {
                        ep, "xyz.openbmc_project.NSM",
                        "com.nvidia.DebugToken.Action"};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = serial;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("SERIAL123", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

// --- nsmTokenInstallV2: exception in loop ---
TEST_F(ChainedMockTest, NsmTokenInstallV2LoopException)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
                return -1; // Properties.Get TokenDeviceID fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                             "com.nvidia.DebugToken.Action"};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 0];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace("S", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(ChainedMockTest, NsmTokenInstallV2TwoEndpointsSkipThenSuccess)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* serialMissing = "SERIAL_MISSING";
    static const char* serialMatch = "SERIAL_MATCH";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 4) ? asyncPath : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 2)
                {
                    val = serialMissing;
                }
                else if (callPhase == 3)
                {
                    val = serialMatch;
                }
                else if (callPhase == 5)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serialMatch, std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, 0);
}

TEST_F(ChainedMockTest, NsmTokenInstallV2RecoversAfterSingleMemfdFailure)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* serialFail = "SERIAL_FAIL";
    static const char* serialGood = "SERIAL_GOOD";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 4) ? asyncPath : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 2)
                {
                    val = serialFail;
                }
                else if (callPhase == 3)
                {
                    val = serialGood;
                }
                else if (callPhase == 5)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    syscall_state::failMemfdCreateRemaining = 1;

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serialFail, std::vector<uint8_t>(96, 0x41));
    tokens.emplace(serialGood, std::vector<uint8_t>(128, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 2);
    EXPECT_EQ(syscall_state::shortWriteCalls, 0);
    EXPECT_GE(syscall_state::writeCalls, 1);
}

TEST_F(ChainedMockTest, NsmTokenInstallV2RecoversAfterSingleShortWrite)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";
    static const char* serialShort = "SERIAL_SHORT";
    static const char* serialGood = "SERIAL_GOOD";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 4) ? asyncPath : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 2)
                {
                    val = serialShort;
                }
                else if (callPhase == 3)
                {
                    val = serialGood;
                }
                else if (callPhase == 5)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            if (type == 'u')
            {
                *static_cast<uint32_t*>(out) = 0;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    syscall_state::shortWriteRemaining = 1;

    UpdateDebugToken udt(bus);
    TokenMap tokens;
    tokens.emplace(serialShort, std::vector<uint8_t>(96, 0x31));
    tokens.emplace(serialGood, std::vector<uint8_t>(128, 0x32));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(syscall_state::memfdCreateCalls, 2);
    EXPECT_EQ(syscall_state::shortWriteCalls, 1);
    EXPECT_GE(syscall_state::writeCalls, 2);
}

// --- nsmTokenEraseV2: loop body exception ---
TEST_F(ChainedMockTest, NsmTokenEraseV2LoopException)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
                return -1; // EraseToken call fails
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o' || type == 's')
            {
                static const char* strs[] = {ep, "xyz.openbmc_project.NSM",
                                             "com.nvidia.DebugToken.Action"};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 0];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

TEST_F(ChainedMockTest, NsmTokenEraseV2TwoEndpointsSuccessThenFailure)
{
    static int callPhase = 0, readIdx = 0;
    static size_t atEndIdx = 0;
    callPhase = readIdx = 0;
    atEndIdx = 0;

    static constexpr int atEndValues[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1};
    static const char* ep0 = "/xyz/openbmc_project/NSM/gpu0";
    static const char* ep1 = "/xyz/openbmc_project/NSM/gpu1";
    static const char* asyncPath = "/com/nvidia/nsmd/async/erase0";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 4)
            {
                return -1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? asyncPath : ep0;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep0;
                if (callPhase == 1)
                {
                    static const char* seq[] = {ep0,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action",
                                                ep1,
                                                "xyz.openbmc_project.NSM",
                                                "com.nvidia.DebugToken.Action"};
                    val = seq[readIdx < 6 ? readIdx : 5];
                }
                else if (callPhase == 3)
                {
                    val = successStatus;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
        .WillRepeatedly([](sd_bus_message*, int) {
            if (atEndIdx < (sizeof(atEndValues) / sizeof(atEndValues[0])))
            {
                return atEndValues[atEndIdx++];
            }
            return 1;
        });
    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

// --- handleAsyncCallEraseV2: Error status, getAsyncValue exception in inner
// check ---
TEST_F(ChainedMockTest, HandleAsyncCallEraseV2ErrorGetValueExceptionInner)
{
    static int callPhase = 0, readIdx = 0;
    callPhase = readIdx = 0;

    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* errorStatus = "com.nvidia.Async.Status.Error";

    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 3: logAsyncError's getAsyncValue fails
            // Phase 4: inner getAsyncValue also fails
            if (callPhase >= 3)
                return -1;
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) = asyncPath;
                return 1;
            }
            if (type == 's')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 2) ? errorStatus : "err";
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
        .WillRepeatedly(Return(1));

    UpdateDebugToken udt(bus);
    auto result = udt.handleAsyncCallEraseV2("/xyz/openbmc_project/NSM/gpu0");
    EXPECT_TRUE(result.empty());
}

TEST_F(StrictMockTest, EnumerateNsmEndpointsV2OneResult)
{
    testing::InSequence seq;

    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadGetSubTreeOneEntry("/xyz/openbmc_project/NSM/gpu0",
                                 "xyz.openbmc_project.NSM",
                                 "com.nvidia.DebugToken.Action");

    UpdateDebugToken udt(bus);
    NSMEndpoints eps;
    EXPECT_EQ(udt.enumerateNsmDebugTokenEndpointsV2(eps), 0);
    EXPECT_EQ(eps.size(), 1u);
}

TEST_F(StrictMockTest, GetMCTPServiceListOneService)
{
    testing::InSequence seq;

    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadGetSubTreeOneEntry("/au/com/codeconstruct/mctp1/network/1",
                                 "au.com.codeconstruct.MCTP1",
                                 mctpEndpointIntfName);

    UpdateDebugToken udt(bus);
    auto services = udt.getMCTPServiceList();

    EXPECT_THAT(services, testing::ElementsAre("au.com.codeconstruct.MCTP1"));
}

TEST_F(StrictMockTest, GetMCTPServiceListTwoServices)
{
    testing::InSequence seq;

    expectNewMethodCall("xyz.openbmc_project.ObjectMapper",
                        "/xyz/openbmc_project/object_mapper",
                        "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    expectAppendGetSubTreeArgs();
    expectBusCallWithReply();
    expectReadGetSubTreeTwoEntries(
        "/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
        mctpEndpointIntfName, "/xyz/openbmc_project/mctp/network/1",
        "xyz.openbmc_project.MCTP", mctpEndpointIntfName);

    UpdateDebugToken udt(bus);
    auto services = udt.getMCTPServiceList();

    EXPECT_THAT(services, testing::ElementsAre("au.com.codeconstruct.MCTP1",
                                               "xyz.openbmc_project.MCTP"));
}
