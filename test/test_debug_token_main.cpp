/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#include <new>
#include <optional>

#define UpdateDebugToken FakeUpdateDebugToken
#define main debug_token_main
#include "../debug_token/main.cpp"
#undef main
#undef UpdateDebugToken

namespace debug_token_main_mock
{

struct ResourceErrorCall
{
    std::string messageId;
    std::string componentName;
    OperationType operationType;
    int errorCode = 0;
    std::string deviceName;
};

struct MessageRegistryCall
{
    std::string messageId;
    std::string componentName;
    std::string version;
};

inline DebugTokenInstallStatus nextInstallStatus =
    DebugTokenInstallStatus::DebugTokenInstallSuccess;
inline int nextEraseStatus = 0;
inline std::optional<ResourceErrorCall> lastResourceErrorCall;
inline std::optional<MessageRegistryCall> lastMessageRegistryCall;
inline std::string lastInstallPath;
inline int installCallCount = 0;
inline int eraseCallCount = 0;
inline bool throwFromErase = false;
inline bool throwFromInstall = false;
inline bool throwFromCreateMessageRegistry = false;
inline bool throwFromCreateResourceError = false;

void reset()
{
    nextInstallStatus = DebugTokenInstallStatus::DebugTokenInstallSuccess;
    nextEraseStatus = 0;
    lastResourceErrorCall.reset();
    lastMessageRegistryCall.reset();
    lastInstallPath.clear();
    installCallCount = 0;
    eraseCallCount = 0;
    throwFromErase = false;
    throwFromInstall = false;
    throwFromCreateMessageRegistry = false;
    throwFromCreateResourceError = false;
}

} // namespace debug_token_main_mock

DebugTokenInstallStatus
    FakeUpdateDebugToken::installDebugToken(const std::string& debugTokenPath)
{
    debug_token_main_mock::installCallCount++;
    debug_token_main_mock::lastInstallPath = debugTokenPath;
    if (debug_token_main_mock::throwFromInstall)
    {
        throw std::runtime_error("install failure");
    }
    return debug_token_main_mock::nextInstallStatus;
}

int FakeUpdateDebugToken::eraseDebugToken()
{
    debug_token_main_mock::eraseCallCount++;
    if (debug_token_main_mock::throwFromErase)
    {
        throw std::runtime_error("erase failure");
    }
    return debug_token_main_mock::nextEraseStatus;
}

void FakeUpdateDebugToken::createMessageRegistry(const std::string& messageID,
                                                 const std::string& compName,
                                                 const std::string& compVersion)
{
    if (debug_token_main_mock::throwFromCreateMessageRegistry)
    {
        throw std::runtime_error("message registry failure");
    }
    debug_token_main_mock::lastMessageRegistryCall = {
        messageID,
        compName,
        compVersion,
    };
}

void FakeUpdateDebugToken::createMessageRegistryResourceErrors(
    const std::string& messageID, const std::string& componentName,
    const OperationType& operationType, const int& errorCode,
    const std::string deviceName)
{
    if (debug_token_main_mock::throwFromCreateResourceError)
    {
        throw std::runtime_error("resource error failure");
    }
    debug_token_main_mock::lastResourceErrorCall = {
        messageID, componentName, operationType, errorCode, deviceName,
    };
}

extern "C" int __real_sd_bus_open_system(sd_bus** bus);

extern "C" int __wrap_sd_bus_open_system(sd_bus** bus)
{
    *bus = nullptr;
    return 0;
}

extern "C" FILE* __wrap_popen(const char*, const char*)
{
    return nullptr;
}

extern "C" int __wrap_pclose(FILE*)
{
    return 0;
}

class DebugTokenMainTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        debug_token_main_mock::reset();
    }
};

TEST_F(DebugTokenMainTest, TooFewArguments)
{
    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("0")};
    EXPECT_EQ(debug_token_main(2, argv), -1);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 0);
    EXPECT_EQ(debug_token_main_mock::installCallCount, 0);
}

TEST_F(DebugTokenMainTest, SingleArgument)
{
    char* argv[] = {const_cast<char*>("updateDebugToken")};
    EXPECT_EQ(debug_token_main(1, argv), -1);
}

TEST_F(DebugTokenMainTest, InvalidArgument)
{
    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("abc"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), -1);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 0);
}

TEST_F(DebugTokenMainTest, InvalidOperationFallsThrough)
{
    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("99"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), 0);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 0);
    EXPECT_EQ(debug_token_main_mock::installCallCount, 0);
}

TEST_F(DebugTokenMainTest, EraseSuccess)
{
    debug_token_main_mock::nextEraseStatus = 0;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("0"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), 0);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 1);
    EXPECT_FALSE(debug_token_main_mock::lastResourceErrorCall.has_value());
}

TEST_F(DebugTokenMainTest, EraseFailureLogsResourceError)
{
    debug_token_main_mock::nextEraseStatus = -1;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("0"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), 0);
    ASSERT_TRUE(debug_token_main_mock::lastResourceErrorCall.has_value());
    EXPECT_EQ(debug_token_main_mock::lastResourceErrorCall->messageId,
              debugTokenEraseFailed);
    EXPECT_EQ(debug_token_main_mock::lastResourceErrorCall->componentName,
              DEBUG_TOKEN_ERASE_NAME);
    EXPECT_EQ(debug_token_main_mock::lastResourceErrorCall->operationType,
              OperationType::TokenErase);
    EXPECT_EQ(debug_token_main_mock::lastResourceErrorCall->errorCode,
              static_cast<int>(EraseErrorCodes::EraseFailed));
}

TEST_F(DebugTokenMainTest, InstallTokenPathWrongArgCount)
{
    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), -1);
    EXPECT_EQ(debug_token_main_mock::installCallCount, 0);
}

TEST_F(DebugTokenMainTest, InstallFailureExitsNonZero)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallFailed;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    // A non-zero exit fails the oneshot debug-token-update@.service job, which
    // is what drives Activation::Failed and the Redfish TaskState Exception.
    EXPECT_EQ(debug_token_main(4, argv), -1);
    EXPECT_EQ(debug_token_main_mock::installCallCount, 1);
    EXPECT_EQ(debug_token_main_mock::lastInstallPath, "/tmp/debug-token.bin");
    // Version::onUpdateFailed() -> logTransferFailed() now emits the
    // Update.1.0.TransferFailed entry, so updateDebugToken must not emit a
    // second, identical one.
    EXPECT_FALSE(debug_token_main_mock::lastMessageRegistryCall.has_value());
}

TEST_F(DebugTokenMainTest, InstallNoneExitsNonZero)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallNone;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    // Regression guard: a token package that matches no device on this
    // system must not report a successful update.
    EXPECT_EQ(debug_token_main(4, argv), -1);
    EXPECT_FALSE(debug_token_main_mock::lastMessageRegistryCall.has_value());
}

TEST_F(DebugTokenMainTest, InstallSuccessLogsUpdateSuccessful)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallSuccess;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    EXPECT_EQ(debug_token_main(4, argv), 0);
    ASSERT_TRUE(debug_token_main_mock::lastMessageRegistryCall.has_value());
    EXPECT_EQ(debug_token_main_mock::lastMessageRegistryCall->messageId,
              updateSuccessful);
    EXPECT_EQ(debug_token_main_mock::lastMessageRegistryCall->componentName,
              DEBUG_TOKEN_INSTALL_NAME);
    EXPECT_EQ(debug_token_main_mock::lastMessageRegistryCall->version, "1.0");
}

TEST_F(DebugTokenMainTest, EraseOperationExceptionReturnsFailure)
{
    debug_token_main_mock::throwFromErase = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("0"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), -1);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 1);
}

TEST_F(DebugTokenMainTest, EraseFailureResourceErrorExceptionReturnsFailure)
{
    debug_token_main_mock::nextEraseStatus = -1;
    debug_token_main_mock::throwFromCreateResourceError = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("0"), const_cast<char*>("1.0")};
    EXPECT_EQ(debug_token_main(3, argv), -1);
    EXPECT_EQ(debug_token_main_mock::eraseCallCount, 1);
}

TEST_F(DebugTokenMainTest, InstallOperationExceptionReturnsFailure)
{
    debug_token_main_mock::throwFromInstall = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    EXPECT_EQ(debug_token_main(4, argv), -1);
    EXPECT_EQ(debug_token_main_mock::installCallCount, 1);
}

TEST_F(DebugTokenMainTest, InstallFailedRegistryExceptionReturnsFailure)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallFailed;
    debug_token_main_mock::throwFromCreateMessageRegistry = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    // The failure branch no longer calls createMessageRegistry() at all, so
    // the armed exception must not fire; the -1 comes from the branch itself.
    EXPECT_EQ(debug_token_main(4, argv), -1);
    EXPECT_FALSE(debug_token_main_mock::lastMessageRegistryCall.has_value());
}

TEST_F(DebugTokenMainTest, InstallNoneRegistryExceptionReturnsFailure)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallNone;
    debug_token_main_mock::throwFromCreateMessageRegistry = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    EXPECT_EQ(debug_token_main(4, argv), -1);
    EXPECT_FALSE(debug_token_main_mock::lastMessageRegistryCall.has_value());
}

TEST_F(DebugTokenMainTest, InstallSuccessRegistryExceptionReturnsFailure)
{
    debug_token_main_mock::nextInstallStatus =
        DebugTokenInstallStatus::DebugTokenInstallSuccess;
    debug_token_main_mock::throwFromCreateMessageRegistry = true;

    char* argv[] = {const_cast<char*>("updateDebugToken"),
                    const_cast<char*>("1"), const_cast<char*>("1.0"),
                    const_cast<char*>("/tmp/debug-token.bin")};
    EXPECT_EQ(debug_token_main(4, argv), -1);
}
