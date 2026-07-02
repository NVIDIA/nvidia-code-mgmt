/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Full D-Bus success path coverage for nsm_debug_token.cpp and
 * update_debug_token.cpp using queue-based SdBusMock response system.
 *
 * Each sd_bus_call advances a "call phase". read_basic returns data
 * appropriate for that phase.
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
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <deque>
#include <filesystem>
#include <fstream>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

// ========================== Queue-based response system ====================
// Each sd_bus_call increments a phase counter. Subsequent read_basic calls
// return data appropriate for that phase.

struct ReadResponse
{
    std::deque<std::pair<char, std::string>> values; // (type, value) pairs
};

void appendAsyncStatusSignal(sd_bus_message* rawMsg, const std::string& status,
                             const std::string& propertyName = "Status")
{
    const char* interface = nsmAsyncStatusIntfName;
    const char* property = propertyName.c_str();
    const char* rawStatus = status.c_str();

    ASSERT_GE(sd_bus_message_append_basic(rawMsg, 's', interface), 0);
    ASSERT_GE(sd_bus_message_open_container(rawMsg, 'a', "{sv}"), 0);
    ASSERT_GE(sd_bus_message_open_container(rawMsg, 'e', "sv"), 0);
    ASSERT_GE(sd_bus_message_append_basic(rawMsg, 's', property), 0);
    ASSERT_GE(sd_bus_message_open_container(rawMsg, 'v', "s"), 0);
    ASSERT_GE(sd_bus_message_append_basic(rawMsg, 's', rawStatus), 0);
    ASSERT_GE(sd_bus_message_close_container(rawMsg), 0);
    ASSERT_GE(sd_bus_message_close_container(rawMsg), 0);
    ASSERT_GE(sd_bus_message_close_container(rawMsg), 0);
    ASSERT_GE(sd_bus_message_open_container(rawMsg, 'a', "s"), 0);
    ASSERT_GE(sd_bus_message_close_container(rawMsg), 0);
}

void invokeAsyncMatch(sd_bus_message_handler_t callback, void* userdata,
                      const std::string& objectPath, const std::string& status)
{
    auto signalBus = sdbusplus::bus::new_default();
    auto msg = signalBus.new_signal(objectPath.c_str(),
                                    "org.freedesktop.DBus.Properties",
                                    "PropertiesChanged");
    auto* rawMsg = msg.release();
    appendAsyncStatusSignal(rawMsg, status);
    ASSERT_GE(sd_bus_message_seal(rawMsg, 0, 0), 0);
    sd_bus_message_rewind(rawMsg, true);
    ASSERT_NE(callback, nullptr);
    EXPECT_GE(callback(rawMsg, userdata, nullptr), 0);
    sd_bus_message_unref(rawMsg);
}

void invokeAsyncMatchWithProperty(sd_bus_message_handler_t callback,
                                  void* userdata, const std::string& objectPath,
                                  const std::string& status,
                                  const std::string& propertyName)
{
    auto signalBus = sdbusplus::bus::new_default();
    auto msg = signalBus.new_signal(objectPath.c_str(),
                                    "org.freedesktop.DBus.Properties",
                                    "PropertiesChanged");
    auto* rawMsg = msg.release();
    appendAsyncStatusSignal(rawMsg, status, propertyName);
    ASSERT_GE(sd_bus_message_seal(rawMsg, 0, 0), 0);
    sd_bus_message_rewind(rawMsg, true);
    ASSERT_NE(callback, nullptr);
    EXPECT_GE(callback(rawMsg, userdata, nullptr), 0);
    sd_bus_message_unref(rawMsg);
}

class FullCoverageTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> mock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    UpdateDebugToken udt{bus};

    int callPhase_ = 0;
    std::deque<ReadResponse> callResponses_;
    int readIdx_ = 0; // index within current phase's values

    void SetUp() override
    {
        callPhase_ = 0;
        readIdx_ = 0;
        callResponses_.clear();

        // sd_bus_call: advance phase, return success
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, _, _, _))
            .WillRepeatedly([this](sd_bus*, sd_bus_message*, uint64_t,
                                   sd_bus_error*, sd_bus_message** reply) {
                if (reply)
                    *reply = nullptr;
                callPhase_++;
                readIdx_ = 0; // reset read index for new phase
                return 0;
            });

        // read_basic: return queued data for current phase
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, _, _))
            .WillRepeatedly([this](sd_bus_message*, char type, void* output) {
                int phase = callPhase_ - 1; // 0-based
                if (phase >= 0 &&
                    phase < static_cast<int>(callResponses_.size()))
                {
                    auto& resp = callResponses_[phase];
                    if (readIdx_ < static_cast<int>(resp.values.size()))
                    {
                        auto& [vtype, val] = resp.values[readIdx_];
                        if (type == 'o' || type == 's')
                        {
                            // Store string in a static buffer so pointer
                            // remains valid
                            static thread_local std::string buf;
                            buf = val;
                            *static_cast<const char**>(output) = buf.c_str();
                            readIdx_++;
                            return 1;
                        }
                    }
                }
                // Default: return 0 (no data)
                if (type == 'o' || type == 's')
                {
                    static const char* empty = "";
                    *static_cast<const char**>(output) = empty;
                    return 1;
                }
                return 0;
            });

        // Container operations always succeed
        EXPECT_CALL(mock, sd_bus_message_enter_container(nullptr, _, _))
            .WillRepeatedly(Return(0));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillRepeatedly(Return(0));

        // verify_type returns 1 (match)
        EXPECT_CALL(mock, sd_bus_message_verify_type(nullptr, _, _))
            .WillRepeatedly(Return(1));

        // at_end: first 2 calls return 0 (has data), rest return 1 (end)
        atEndIdx_ = 0;
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, _))
            .WillRepeatedly([this](sd_bus_message*, int) {
                return (atEndIdx_++ < 2) ? 0 : 1;
            });

        EXPECT_CALL(mock, sd_bus_message_skip(nullptr, _))
            .WillRepeatedly(Return(0));

        EXPECT_CALL(mock, sd_bus_message_get_type(nullptr, _))
            .WillRepeatedly([](sd_bus_message*, uint8_t* type) {
                *type = 2;
                return 0;
            });

        EXPECT_CALL(mock, sd_bus_message_get_signature(nullptr, _))
            .WillRepeatedly(Return("s"));
    }

    int atEndIdx_ = 0;

    // Helper: add a response for the next sd_bus_call
    void addResponse(std::deque<std::pair<char, std::string>> values)
    {
        callResponses_.push_back({std::move(values)});
    }

    // Helper: add a GetSubTree response with one endpoint
    void addGetSubTreeResponse(const std::string& path,
                               const std::string& service)
    {
        addResponse({{'o', path}, {'s', service}});
    }

    // Helper: add a Get property response (variant<string>)
    void addPropertyResponse(const std::string& value)
    {
        addResponse({{'s', value}});
    }

    // Helper: add a method call response returning object path
    void addObjectPathResponse(const std::string& path)
    {
        addResponse({{'o', path}});
    }
};

// ========================== getErasePolicy success paths ===================

TEST_F(FullCoverageTest, GetErasePolicyManual)
{
    addGetSubTreeResponse("/com/nvidia/debug_token/policy",
                          "com.nvidia.DebugToken");
    addPropertyResponse("Manual");

    auto policy = udt.getErasePolicy();
    // If the mock works, this returns "Manual" and covers lines 88-140
    (void)policy;
}

TEST_F(FullCoverageTest, GetErasePolicyAutomatic)
{
    addGetSubTreeResponse("/com/nvidia/debug_token/policy",
                          "com.nvidia.DebugToken");
    addPropertyResponse("Automatic");

    auto policy = udt.getErasePolicy();
    (void)policy;
}

// ========================== eraseDebugToken with Manual policy =============

TEST_F(FullCoverageTest, EraseDebugTokenManualPolicy)
{
    // Phase 1: getErasePolicy -> GetSubTree
    addGetSubTreeResponse("/com/nvidia/debug_token/policy",
                          "com.nvidia.DebugToken");
    // Phase 2: getErasePolicy -> Get property
    addPropertyResponse("Manual");

    int result = udt.eraseDebugToken();
    // With Manual policy, eraseDebugToken skips the operation and returns the
    // distinct "skipped" code (not success) so the caller won't log success.
    EXPECT_EQ(result, eraseTokenSkipped);
}

// ========================== nsmTokenErase with endpoints ===================

TEST_F(FullCoverageTest, NsmTokenEraseOneEndpointNoToken)
{
    // Phase 1: enumerateNsmDebugTokenEndpoints -> GetSubTree
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // Phase 2: getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    // Phase 3: handleAsyncCall -> Get initial status
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Phase 4: getAsyncValue -> Get
    addPropertyResponse("NoTokenApplied");

    int result = udt.nsmTokenErase();
    (void)result;
}

// ========================== nsmTokenInstall with endpoints =================

TEST_F(FullCoverageTest, NsmTokenInstallOneEndpointNoMatch)
{
    // Phase 1: enumerateNsmDebugTokenEndpoints -> GetSubTree
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // Phase 2: getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    // Phase 3: handleAsyncCall -> Get initial status
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Phase 4: getAsyncValue -> Get (token status tuple)
    addPropertyResponse("NoTokenApplied");
    // Phase 5: nsmTokenInstall -> Get TokenDeviceID
    addPropertyResponse("SN_NOT_IN_MAP");

    TokenMap tokens;
    tokens.emplace("SN_DIFFERENT", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

TEST_F(FullCoverageTest, NsmTokenInstallOneEndpointWithMatch)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse("NoTokenApplied");
    // TokenDeviceID matches a key in tokens map
    addPropertyResponse("SN_MATCH");
    // handleAsyncCall for InstallToken
    addObjectPathResponse("/com/nvidia/nsmd/async/2");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Verify status after install
    addObjectPathResponse("/com/nvidia/nsmd/async/3");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse("DebugSessionActive");

    TokenMap tokens;
    // Need 45+ bytes (44 stripped as header)
    tokens.emplace("SN_MATCH", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// ========================== nsmTokenEraseV2 with endpoints =================

TEST_F(FullCoverageTest, NsmTokenEraseV2OneEndpoint)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // handleAsyncCallEraseV2 -> makeDebugTokenMethodCall (EraseToken V2)
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    (void)result;
}

// ========================== nsmTokenInstallV2 with endpoints ===============

TEST_F(FullCoverageTest, NsmTokenInstallV2OneEndpoint)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // Get TokenDeviceID
    addPropertyResponse("SN_V2_MATCH");
    // handleAsyncCallInstallV2 -> bus.call for InstallToken
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("SN_V2_MATCH", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// ========================== handleAsyncCall full path ======================

TEST_F(FullCoverageTest, HandleAsyncCallSuccess)
{
    // makeDebugTokenMethodCall returns async path
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    // Get initial status -> Success (not InProgress)
    addPropertyResponse("com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCall("/test/path", "EraseToken");
    (void)result;
}

TEST_F(FullCoverageTest, HandleAsyncCallInProgressThenSuccess)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/full_cov_1";
    addObjectPathResponse(asyncPath);
    // Initial status -> InProgress (triggers polling loop)
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed1);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCall("/test/path", "DisableTokens");
    EXPECT_EQ(result, asyncPath);
}

TEST_F(FullCoverageTest, HandleAsyncCallWrongPathSignalThenSuccess)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/full_cov_2";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed11);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata,
                         "/com/nvidia/nsmd/AsyncOperation/other",
                         "com.nvidia.Async.Status.Success");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCall("/test/path", "DisableTokens");
    EXPECT_EQ(result, asyncPath);
}

TEST_F(FullCoverageTest, HandleAsyncCallMissingStatusSignalThenSuccess)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/full_cov_3";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed12);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatchWithProperty(matchCallback, matchUserdata, asyncPath,
                                     "com.nvidia.Async.Status.Success",
                                     "Other");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCall("/test/path", "DisableTokens");
    EXPECT_EQ(result, asyncPath);
}

TEST_F(FullCoverageTest, HandleAsyncCallWrongPathSignalTimesOut)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/full_cov_4";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed17);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    bool signaled = false;
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillRepeatedly([&](sd_bus*, uint64_t) {
        if (!signaled)
        {
            invokeAsyncMatch(matchCallback, matchUserdata,
                             "/com/nvidia/nsmd/AsyncOperation/other",
                             "com.nvidia.Async.Status.Success");
            signaled = true;
        }
        return 0;
    });

    auto result = udt.handleAsyncCall("/test/path", "DisableTokens");
    EXPECT_TRUE(result.empty());
}

TEST_F(FullCoverageTest, HandleAsyncCallMissingStatusSignalTimesOut)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/full_cov_5";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed18);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    bool signaled = false;
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillRepeatedly([&](sd_bus*, uint64_t) {
        if (!signaled)
        {
            invokeAsyncMatchWithProperty(
                matchCallback, matchUserdata, asyncPath,
                "com.nvidia.Async.Status.Success", "Other");
            signaled = true;
        }
        return 0;
    });

    auto result = udt.handleAsyncCall("/test/path", "DisableTokens");
    EXPECT_TRUE(result.empty());
}

TEST_F(FullCoverageTest, HandleAsyncCallFailed)
{
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Failed");

    auto result = udt.handleAsyncCall("/test/path", "InstallToken",
                                      std::vector<uint8_t>{0x01});
    (void)result;
}

// ========================== handleAsyncCallInstallV2 ======================

TEST_F(FullCoverageTest, HandleAsyncCallInstallV2Success)
{
    int memfd = memfd_create("token", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(256, 0xCC);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);

        addObjectPathResponse("/com/nvidia/nsmd/async/1");
        addPropertyResponse("com.nvidia.Async.Status.Success");

        auto result = udt.handleAsyncCallInstallV2("/test", memfd);
        (void)result;
        close(memfd);
    }
}

TEST_F(FullCoverageTest, HandleAsyncCallInstallV2InProgressThenSuccess)
{
    int memfd = memfd_create("token-progress", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(128, 0xCC);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_install";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed2);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCallInstallV2("/test/path", memfd);
    EXPECT_EQ(result, asyncPath);
    close(memfd);
}

TEST_F(FullCoverageTest, HandleAsyncCallInstallV2WrongPathSignalThenSuccess)
{
    int memfd = memfd_create("token-progress-wrong-path", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(96, 0xAA);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_install_2";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed13);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata,
                         "/com/nvidia/nsmd/AsyncOperation/other",
                         "com.nvidia.Async.Status.Success");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCallInstallV2("/test/path", memfd);
    EXPECT_EQ(result, asyncPath);
    close(memfd);
}

TEST_F(FullCoverageTest, HandleAsyncCallInstallV2MissingStatusThenSuccess)
{
    int memfd = memfd_create("token-progress-missing-status", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(96, 0xAC);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_install_3";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed14);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatchWithProperty(matchCallback, matchUserdata, asyncPath,
                                     "com.nvidia.Async.Status.Success",
                                     "Other");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCallInstallV2("/test/path", memfd);
    EXPECT_EQ(result, asyncPath);
    close(memfd);
}

TEST_F(FullCoverageTest, HandleAsyncCallInstallV2WrongPathSignalTimesOut)
{
    int memfd = memfd_create("token-progress-timeout-wrong-path", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(96, 0xAD);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_install_4";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed19);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    bool signaled = false;
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillRepeatedly([&](sd_bus*, uint64_t) {
        if (!signaled)
        {
            invokeAsyncMatch(matchCallback, matchUserdata,
                             "/com/nvidia/nsmd/AsyncOperation/other",
                             "com.nvidia.Async.Status.Success");
            signaled = true;
        }
        return 0;
    });

    auto result = udt.handleAsyncCallInstallV2("/test/path", memfd);
    EXPECT_TRUE(result.empty());
    close(memfd);
}

// ========================== handleAsyncCallEraseV2 ========================

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2Success)
{
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    (void)result;
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2Failed)
{
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Failed");

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    (void)result;
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2InProgressThenNotInstalled)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_erase";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");
    addPropertyResponse("token missing");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed3);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Error");
        return 0;
    });

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    (void)result;
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2WrongPathSignalThenSuccess)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_erase_2";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed15);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatch(matchCallback, matchUserdata,
                         "/com/nvidia/nsmd/AsyncOperation/other",
                         "com.nvidia.Async.Status.Success");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    EXPECT_EQ(result, asyncPath);
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2MissingStatusThenSuccess)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_erase_3";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed16);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillOnce([&](sd_bus*, uint64_t) {
        invokeAsyncMatchWithProperty(matchCallback, matchUserdata, asyncPath,
                                     "com.nvidia.Async.Status.Success",
                                     "Other");
        invokeAsyncMatch(matchCallback, matchUserdata, asyncPath,
                         "com.nvidia.Async.Status.Success");
        return 0;
    });

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    EXPECT_EQ(result, asyncPath);
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2WrongPathSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_erase_4";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed20);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    bool signaled = false;
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillRepeatedly([&](sd_bus*, uint64_t) {
        if (!signaled)
        {
            invokeAsyncMatch(matchCallback, matchUserdata,
                             "/com/nvidia/nsmd/AsyncOperation/other",
                             "com.nvidia.Async.Status.Success");
            signaled = true;
        }
        return 0;
    });

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    EXPECT_TRUE(result.empty());
}

TEST_F(FullCoverageTest, HandleAsyncCallEraseV2MissingStatusSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/full_cov_erase_5";
    addObjectPathResponse(asyncPath);
    addPropertyResponse("com.nvidia.Async.Status.InProgress");

    sd_bus_message_handler_t matchCallback = nullptr;
    void* matchUserdata = nullptr;
    auto* fakeSlot = reinterpret_cast<sd_bus_slot*>(0xfeed21);
    EXPECT_CALL(mock, sd_bus_add_match(_, _, _, _, _))
        .WillOnce([&](sd_bus*, sd_bus_slot** slot, const char*,
                      sd_bus_message_handler_t callback, void* userdata) {
            matchCallback = callback;
            matchUserdata = userdata;
            if (slot)
            {
                *slot = fakeSlot;
            }
            return 0;
        });
    bool signaled = false;
    EXPECT_CALL(mock, sd_bus_wait(_, _)).WillRepeatedly([&](sd_bus*, uint64_t) {
        if (!signaled)
        {
            invokeAsyncMatchWithProperty(
                matchCallback, matchUserdata, asyncPath,
                "com.nvidia.Async.Status.Success", "Other");
            signaled = true;
        }
        return 0;
    });

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    EXPECT_TRUE(result.empty());
}

// ========================== logAsyncError paths ============================

TEST_F(FullCoverageTest, LogAsyncErrorNotInstalled)
{
    // getAsyncValue returns NSMErrorTuple with NotInstalled code
    addPropertyResponse(""); // will throw during read -> catches

    EXPECT_NO_THROW(udt.logAsyncError("/test", "EraseToken", "Error"));
}

// ========================== getTokenStatus paths ==========================

TEST_F(FullCoverageTest, GetTokenStatusSuccess)
{
    // handleAsyncCall -> makeDebugTokenMethodCall returns path
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // getAsyncValue returns token status tuple
    addPropertyResponse("DebugSessionActive");

    auto result = udt.getTokenStatus("/test");
    (void)result;
}

// ========================== getMCTPServiceList success =====================

TEST_F(FullCoverageTest, GetMCTPServiceListSuccess)
{
    addGetSubTreeResponse("/au/com/codeconstruct/mctp1",
                          "au.com.codeconstruct.MCTP1");

    auto services = udt.getMCTPServiceList();
    (void)services;
}

// ========================== discoverMCTPDevices paths ======================

TEST_F(FullCoverageTest, DiscoverMCTPDevicesNoObjects)
{
    // getMCTPServiceList returns empty -> getMCTPManagedObjects empty
    int result = udt.discoverMCTPDevices();
    (void)result;
}

// ========================== update_debug_token.cpp remaining paths =========

TEST_F(FullCoverageTest, GetErasePolicyEmpty)
{
    // No responses queued -> D-Bus calls fail -> returns empty
    auto policy = udt.getErasePolicy();
    (void)policy;
}

TEST_F(FullCoverageTest, CreateLogSuccess)
{
    std::map<std::string, std::string> addData;
    addData["key"] = "val";
    Level level = Level::Informational;
    EXPECT_NO_THROW(udt.createLog("test.id", addData, level));
}

TEST_F(FullCoverageTest, CreateMessageRegistrySuccess)
{
    EXPECT_NO_THROW(udt.createMessageRegistry("Update.1.0.UpdateSuccessful",
                                              "Component", "1.0"));
}

TEST_F(FullCoverageTest, CreateMessageRegistryFailure)
{
    EXPECT_NO_THROW(udt.createMessageRegistry("Update.1.0.TransferFailed",
                                              "Component", "1.0"));
}

TEST_F(FullCoverageTest, CreateMsgRegistryResourceErrorsAll)
{
    for (auto op : {OperationType::TokenInstall, OperationType::TokenErase,
                    OperationType::BackgroundCopy, OperationType::Common})
    {
        EXPECT_NO_THROW(udt.createMessageRegistryResourceErrors(
            "ResourceEvent.1.0.ResourceErrorsDetected", "Comp", op, 1, "d"));
    }
}

// ========================== installDebugToken with valid TLV file ==========

TEST_F(FullCoverageTest, InstallDebugTokenValidTlv)
{
    std::string tmpPath = "/tmp/test_full_cov_install.bin";
    {
        debug_token::StructureHeader hdr{};
        std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
        hdr.versionMajor = htole16(2);
        hdr.versionMinor = htole16(0);
        debug_token::ItemHeader itemHdr;
        itemHdr.type = htole16(0x0003);
        itemHdr.size = htole16(8);
        uint8_t serial[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        hdr.size = htole32(sizeof(itemHdr) + 8);

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

    // Queue responses for nsmTokenInstallV2 path
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");

    auto status = udt.installDebugToken(tmpPath);
    (void)status;
    std::filesystem::remove(tmpPath);
}

// ========================== eraseDebugToken full flow ======================

TEST_F(FullCoverageTest, EraseDebugTokenFullFlow)
{
    // getErasePolicy: GetSubTree + Get -> "Automatic"
    addGetSubTreeResponse("/com/nvidia/debug_token/policy",
                          "com.nvidia.DebugToken");
    addPropertyResponse("Automatic");
    // nsmTokenEraseV2: enumerateV2 -> GetSubTree
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // handleAsyncCallEraseV2: makeDebugTokenMethodCall
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    int result = udt.eraseDebugToken();
    (void)result;
}

// ====================== MORE SCENARIOS FOR DEEPER COVERAGE ================

// nsmTokenErase: endpoint with DebugSessionActive status -> DisableTokens
TEST_F(FullCoverageTest, NsmTokenEraseActiveSession)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall (GetStatus)
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusDebugSessionActive);
    // DisableTokens -> handleAsyncCall
    addObjectPathResponse("/com/nvidia/nsmd/async/2");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Verify status after disable
    addObjectPathResponse("/com/nvidia/nsmd/async/3");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusNoTokenApplied);

    int result = udt.nsmTokenErase();
    (void)result;
}

// nsmTokenErase: endpoint with TokenTimeout status -> DisableTokens
TEST_F(FullCoverageTest, NsmTokenEraseTokenTimeout)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusTokenTimeout);
    // DisableTokens
    addObjectPathResponse("/com/nvidia/nsmd/async/2");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Verify
    addObjectPathResponse("/com/nvidia/nsmd/async/3");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusNoTokenApplied);

    int result = udt.nsmTokenErase();
    (void)result;
}

// nsmTokenInstall: endpoint with matching serial -> full install flow
TEST_F(FullCoverageTest, NsmTokenInstallFullFlow)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    // getTokenStatus
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusNoTokenApplied);
    // Get TokenDeviceID
    addPropertyResponse("SERIAL_001");
    // InstallToken -> handleAsyncCall
    addObjectPathResponse("/com/nvidia/nsmd/async/2");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    // Verify after install
    addObjectPathResponse("/com/nvidia/nsmd/async/3");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusDebugSessionActive);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// nsmTokenInstall: endpoint with active session -> skip
TEST_F(FullCoverageTest, NsmTokenInstallActiveSessionSkip)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");
    addPropertyResponse(nsmTokenStatusDebugSessionActive);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// handleAsyncCall: makeDebugTokenMethodCall fails -> return ""
TEST_F(FullCoverageTest, HandleAsyncCallMethodCallFails)
{
    // No responses queued -> bus.call fails -> makeDebugTokenMethodCall throws
    auto result = udt.handleAsyncCall("/test", "EraseToken");
    (void)result;
}

// handleAsyncCallInstallV2 with memfd + failed status
TEST_F(FullCoverageTest, HandleAsyncCallInstallV2Failed)
{
    int memfd = memfd_create("tok", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(64, 0xDD);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);

        addObjectPathResponse("/com/nvidia/nsmd/async/1");
        addPropertyResponse("com.nvidia.Async.Status.Failed");

        auto result = udt.handleAsyncCallInstallV2("/test", memfd);
        (void)result;
        close(memfd);
    }
}

// handleAsyncCallEraseV2 with NotInstalled error
TEST_F(FullCoverageTest, HandleAsyncCallEraseV2NotInstalled)
{
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Error");

    auto result = udt.handleAsyncCallEraseV2("/test/path");
    (void)result;
}

// nsmTokenInstallV2: no matching serial -> skip
TEST_F(FullCoverageTest, NsmTokenInstallV2NoMatch)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addPropertyResponse("DIFFERENT_SERIAL");

    TokenMap tokens;
    tokens.emplace("MY_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// nsmTokenInstallV2: matching serial -> install
TEST_F(FullCoverageTest, NsmTokenInstallV2WithMatch)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addPropertyResponse("MATCH_SERIAL");
    // handleAsyncCallInstallV2 -> bus.call
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("MATCH_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

// nsmTokenEraseV2: one endpoint -> handleAsyncCallEraseV2 success
TEST_F(FullCoverageTest, NsmTokenEraseV2Success)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    (void)result;
}

// nsmTokenEraseV2: one endpoint -> handleAsyncCallEraseV2 fails
TEST_F(FullCoverageTest, NsmTokenEraseV2Failure)
{
    addGetSubTreeResponse("/xyz/openbmc_project/NSM/gpu0",
                          "xyz.openbmc_project.NSM");
    addObjectPathResponse("/com/nvidia/nsmd/async/1");
    addPropertyResponse("com.nvidia.Async.Status.Failed");

    int result = udt.nsmTokenEraseV2();
    (void)result;
}

// getTokenStatus: handleAsyncCall returns empty -> return ""
TEST_F(FullCoverageTest, GetTokenStatusHandleAsyncFails)
{
    // No responses -> handleAsyncCall fails
    auto result = udt.getTokenStatus("/test");
    (void)result;
}

// logAsyncError variants
TEST_F(FullCoverageTest, LogAsyncErrorVariants)
{
    EXPECT_NO_THROW(udt.logAsyncError("/t", "EraseToken", "Failed"));
    EXPECT_NO_THROW(udt.logAsyncError("/t", "InstallToken", "Timeout"));
    EXPECT_NO_THROW(udt.logAsyncError("/t", "DisableTokens", "Error"));
}

// getMCTPServiceList -> getMCTPManagedObjects -> discoverMCTPDevices chain
TEST_F(FullCoverageTest, DiscoverMCTPDevicesWithService)
{
    // getMCTPServiceList: GetSubTree returns one service
    addGetSubTreeResponse("/au/com/codeconstruct/mctp1",
                          "au.com.codeconstruct.MCTP1");
    // getMCTPManagedObjects: GetManagedObjects
    addResponse({}); // empty managed objects

    int result = udt.discoverMCTPDevices();
    (void)result;
}

// updateEndPoints chain
TEST_F(FullCoverageTest, UpdateEndPointsWithService)
{
    // discoverMCTPDevices -> getMCTPServiceList -> GetSubTree
    addGetSubTreeResponse("/au/com/codeconstruct/mctp1",
                          "au.com.codeconstruct.MCTP1");
    addResponse({}); // getMCTPManagedObjects
    // updateEndPoints -> pldm GetManagedObjects
    addResponse({}); // pldm managed objects

    int result = udt.updateEndPoints();
    (void)result;
}

// updateDeviceMap with full interface data
TEST_F(FullCoverageTest, UpdateDeviceMapFull)
{
    dbus::InterfaceMap interfaces;

    // MCTP endpoint
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(42);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{0x05, 0x7f};
    interfaces[mctpEndpointIntfName] = eidProps;

    // UUID
    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("test-uuid");
    interfaces[uuidEndpointIntfName] = uuidProps;

    // PLDM inventory with serial
    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN12345");
    interfaces[pldmInventoryIntfName] = pldmProps;

    EXPECT_NO_THROW(udt.updateDeviceMap(interfaces, "GPU_ERoT_0"));
}

// fetchEidInfoFromObject with all fields
TEST_F(FullCoverageTest, FetchEidInfoComplete)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(10);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{0x05, 0x7f};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindProps;
    bindProps["BindingMediumID"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    bindProps["BindingType"] =
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 10);
    EXPECT_TRUE(info.enabled);
}
