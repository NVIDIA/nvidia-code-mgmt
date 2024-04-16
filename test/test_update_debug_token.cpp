/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "../debug_token/update_debug_token.hpp"

#include <stdlib.h>

#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

class TestUpdateDebugToken : public testing::Test
{
  public:
    std::unique_ptr<UpdateDebugToken> updateDebugToken;
    TestUpdateDebugToken()
    {
        auto bus = sdbusplus::bus::new_default();
        updateDebugToken = std::make_unique<UpdateDebugToken>(bus);
    }

    ~TestUpdateDebugToken()
    {}
};

TEST_F(TestUpdateDebugToken, DebugTokenMapSingle)
{
    SerialNumber expectedSerial = "0x011E020E160A1017";
    Token expectedToken = {
        0x45, 0x44, 0x54, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x46, 0x01, 0x00, 0x00, 0x2c, 0x3d, 0x17, 0x56,
        0xca, 0x72, 0x86, 0x1a, 0x0f, 0xc6, 0x5d, 0x0a, 0x78, 0x27, 0x6c, 0xeb,
        0x01, 0x1e, 0x02, 0x0e, 0x16, 0x0a, 0x10, 0x17, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x87, 0xe9, 0xf7, 0xc7, 0x58, 0x7b, 0x7e, 0x9b,
        0x71, 0xbc, 0xfe, 0xc3, 0x2b, 0x1b, 0x17, 0x04, 0x11, 0xc6, 0xcf, 0xc3,
        0xd9, 0x66, 0x8a, 0xa9, 0x60, 0xa1, 0x9d, 0x82, 0x7a, 0x49, 0x06, 0x03,
        0x0d, 0xcd, 0x95, 0x7c, 0xe7, 0x53, 0xec, 0x3b, 0x0c, 0x14, 0x56, 0x41,
        0x87, 0xaf, 0x2d, 0x69, 0x28, 0x83, 0x60, 0xe8, 0xa8, 0x3a, 0xbd, 0xe9,
        0x45, 0x48, 0xd0, 0x2e, 0xa0, 0xa6, 0x54, 0xa8, 0xf4, 0xc0, 0xa8, 0x78,
        0xdc, 0xcd, 0xd7, 0xa0, 0x7e, 0xcc, 0x82, 0xaf, 0xce, 0x59, 0x4e, 0x8e,
        0x36, 0xbb, 0x1d, 0x31, 0xd8, 0xd8, 0xc7, 0x6d, 0x07, 0x22, 0x8a, 0xeb,
        0xa3, 0x22, 0x62, 0x84, 0xa7, 0x7a, 0xed, 0xa2, 0x08, 0x1a, 0x5c, 0xcb,
        0xaf, 0x94, 0xa1, 0x55, 0x23, 0xf7, 0x8a, 0x0d, 0x72, 0xbf, 0x29, 0xc0,
        0xf5, 0xb4, 0xd8, 0xae, 0x1f, 0x52, 0x41, 0xf4, 0x11, 0x8f, 0xa3, 0xd5,
        0xa1, 0xb6, 0xa2, 0x10, 0xb6, 0x71, 0x11, 0x3f, 0xfd, 0x72, 0x19, 0xba,
        0xf9, 0x5c, 0x6d, 0x2f, 0x85, 0x7c, 0xec, 0xcf, 0xaf, 0xda, 0xba, 0xa4,
        0xaf, 0xc5, 0xbc, 0x13, 0x98, 0x9f, 0xc4, 0x4a, 0x52, 0xa4, 0xf2, 0xdc,
        0x85, 0x34, 0x72, 0x4a, 0x41, 0x5f, 0x57, 0x2e, 0xaa, 0x6a, 0x9f, 0xde,
        0xef, 0xbf, 0x3f, 0xf2, 0x7c, 0x78, 0x65, 0x50, 0x5b, 0x98, 0x80, 0x55,
        0x12, 0xac, 0x9f, 0x43};
    TokenMap tokensExpected = {{expectedSerial, expectedToken}};
    TokenMap tokensParsed;
    if (updateDebugToken->updateTokenMap("./debug_token_single.bin",
                                         tokensParsed) == 0)
    {
        EXPECT_EQ(tokensExpected.size(), tokensParsed.size());
        EXPECT_EQ(tokensExpected, tokensParsed);
    }
    else
    {
        EXPECT_EQ(tokensExpected.size(), tokensParsed.size());
    }
}

TEST_F(TestUpdateDebugToken, DebugTokenMapMultiple)
{
    SerialNumber expectedSerial1 = "0x011E020E160A1017";
    SerialNumber expectedSerial2 = "0x011E020E160A1018";
    Token expectedToken1 = {
        0x45, 0x44, 0x54, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x46, 0x01, 0x00, 0x00, 0x2c, 0x3d, 0x17, 0x56,
        0xca, 0x72, 0x86, 0x1a, 0x0f, 0xc6, 0x5d, 0x0a, 0x78, 0x27, 0x6c, 0xeb,
        0x01, 0x1e, 0x02, 0x0e, 0x16, 0x0a, 0x10, 0x17, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x87, 0xe9, 0xf7, 0xc7, 0x58, 0x7b, 0x7e, 0x9b,
        0x71, 0xbc, 0xfe, 0xc3, 0x2b, 0x1b, 0x17, 0x04, 0x11, 0xc6, 0xcf, 0xc3,
        0xd9, 0x66, 0x8a, 0xa9, 0x60, 0xa1, 0x9d, 0x82, 0x7a, 0x49, 0x06, 0x03,
        0x0d, 0xcd, 0x95, 0x7c, 0xe7, 0x53, 0xec, 0x3b, 0x0c, 0x14, 0x56, 0x41,
        0x87, 0xaf, 0x2d, 0x69, 0x28, 0x83, 0x60, 0xe8, 0xa8, 0x3a, 0xbd, 0xe9,
        0x45, 0x48, 0xd0, 0x2e, 0xa0, 0xa6, 0x54, 0xa8, 0xf4, 0xc0, 0xa8, 0x78,
        0xdc, 0xcd, 0xd7, 0xa0, 0x7e, 0xcc, 0x82, 0xaf, 0xce, 0x59, 0x4e, 0x8e,
        0x36, 0xbb, 0x1d, 0x31, 0xd8, 0xd8, 0xc7, 0x6d, 0x07, 0x22, 0x8a, 0xeb,
        0xa3, 0x22, 0x62, 0x84, 0xa7, 0x7a, 0xed, 0xa2, 0x08, 0x1a, 0x5c, 0xcb,
        0xaf, 0x94, 0xa1, 0x55, 0x23, 0xf7, 0x8a, 0x0d, 0x72, 0xbf, 0x29, 0xc0,
        0xf5, 0xb4, 0xd8, 0xae, 0x1f, 0x52, 0x41, 0xf4, 0x11, 0x8f, 0xa3, 0xd5,
        0xa1, 0xb6, 0xa2, 0x10, 0xb6, 0x71, 0x11, 0x3f, 0xfd, 0x72, 0x19, 0xba,
        0xf9, 0x5c, 0x6d, 0x2f, 0x85, 0x7c, 0xec, 0xcf, 0xaf, 0xda, 0xba, 0xa4,
        0xaf, 0xc5, 0xbc, 0x13, 0x98, 0x9f, 0xc4, 0x4a, 0x52, 0xa4, 0xf2, 0xdc,
        0x85, 0x34, 0x72, 0x4a, 0x41, 0x5f, 0x57, 0x2e, 0xaa, 0x6a, 0x9f, 0xde,
        0xef, 0xbf, 0x3f, 0xf2, 0x7c, 0x78, 0x65, 0x50, 0x5b, 0x98, 0x80, 0x55,
        0x12, 0xac, 0x9f, 0x43};
    Token expectedToken2 = {
        0x45, 0x44, 0x54, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x46, 0x01, 0x00, 0x00, 0x2c, 0x3d, 0x17, 0x56,
        0xca, 0x72, 0x86, 0x1a, 0x0f, 0xc6, 0x5d, 0x0a, 0x78, 0x27, 0x6c, 0xeb,
        0x01, 0x1e, 0x02, 0x0e, 0x16, 0x0a, 0x10, 0x17, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x87, 0xe9, 0xf7, 0xc7, 0x58, 0x7b, 0x7e, 0x9b,
        0x71, 0xbc, 0xfe, 0xc3, 0x2b, 0x1b, 0x17, 0x04, 0x11, 0xc6, 0xcf, 0xc3,
        0xd9, 0x66, 0x8a, 0xa9, 0x60, 0xa1, 0x9d, 0x82, 0x7a, 0x49, 0x06, 0x03,
        0x0d, 0xcd, 0x95, 0x7c, 0xe7, 0x53, 0xec, 0x3b, 0x0c, 0x14, 0x56, 0x41,
        0x87, 0xaf, 0x2d, 0x69, 0x28, 0x83, 0x60, 0xe8, 0xa8, 0x3a, 0xbd, 0xe9,
        0x45, 0x48, 0xd0, 0x2e, 0xa0, 0xa6, 0x54, 0xa8, 0xf4, 0xc0, 0xa8, 0x78,
        0xdc, 0xcd, 0xd7, 0xa0, 0x7e, 0xcc, 0x82, 0xaf, 0xce, 0x59, 0x4e, 0x8e,
        0x36, 0xbb, 0x1d, 0x31, 0xd8, 0xd8, 0xc7, 0x6d, 0x07, 0x22, 0x8a, 0xeb,
        0xa3, 0x22, 0x62, 0x84, 0xa7, 0x7a, 0xed, 0xa2, 0x08, 0x1a, 0x5c, 0xcb,
        0xaf, 0x94, 0xa1, 0x55, 0x23, 0xf7, 0x8a, 0x0d, 0x72, 0xbf, 0x29, 0xc0,
        0xf5, 0xb4, 0xd8, 0xae, 0x1f, 0x52, 0x41, 0xf4, 0x11, 0x8f, 0xa3, 0xd5,
        0xa1, 0xb6, 0xa2, 0x10, 0xb6, 0x71, 0x11, 0x3f, 0xfd, 0x72, 0x19, 0xba,
        0xf9, 0x5c, 0x6d, 0x2f, 0x85, 0x7c, 0xec, 0xcf, 0xaf, 0xda, 0xba, 0xa4,
        0xaf, 0xc5, 0xbc, 0x13, 0x98, 0x9f, 0xc4, 0x4a, 0x52, 0xa4, 0xf2, 0xdc,
        0x85, 0x34, 0x72, 0x4a, 0x41, 0x5f, 0x57, 0x2e, 0xaa, 0x6a, 0x9f, 0xde,
        0xef, 0xbf, 0x3f, 0xf2, 0x7c, 0x78, 0x65, 0x50, 0x5b, 0x98, 0x80, 0x55,
        0x12, 0xac, 0x9f, 0x44};
    TokenMap tokensExpected = {{expectedSerial1, expectedToken1},
                               {expectedSerial2, expectedToken2}};
    TokenMap tokensParsed;
    std::cerr << "Calling parseDebugTokens" << std::endl;
    if (updateDebugToken->updateTokenMap("./debug_token_multiple.bin",
                                         tokensParsed) == 0)
    {
        EXPECT_EQ(tokensExpected.size(), tokensParsed.size());
        for (auto& tokenParsed : tokensParsed)
        {
            if (tokensExpected.find(tokenParsed.first) == tokensExpected.end())
            {
                EXPECT_EQ(tokensExpected, tokensParsed);
                break;
            }
            else
            {
                EXPECT_EQ(tokenParsed.second, tokensParsed[tokenParsed.first]);
            }
        }
    }
    else
    {
        EXPECT_EQ(tokensExpected.size(), tokensParsed.size());
    }
}

TEST_F(TestUpdateDebugToken, TestFormatMessage)
{
    std::string errorMessageInput = "Invalid Debug Token for {}.";
    std::string testDeviceName = "Test_Device_Name_0";
    std::string errorMessageExpected =
        "Invalid Debug Token for Test_Device_Name_0.";
    auto errorMessageOutput =
        updateDebugToken->formatMessage(errorMessageInput, testDeviceName);
    EXPECT_EQ(errorMessageOutput, errorMessageExpected);
}

TEST_F(TestUpdateDebugToken, TestMessageTokenInstall)
{
    std::string testDeviceName = "Test_Device_Name_0";
    OperationType testOperationType;
    int testErrorCode;
    bool expectedMessageStatus = true;
    std::string expectedMessageError;
    std::string expectedResolution;
    std::optional<std::tuple<std::string, std::string>> outputMessage;

    // Token Install
    testOperationType = OperationType::TokenInstall;
    testErrorCode = static_cast<int>(InstallErrorCodes::InvalidToken);
    expectedMessageError =
        installErrorMapping[InstallErrorCodes::InvalidToken].first;
    expectedMessageError =
        updateDebugToken->formatMessage(expectedMessageError, testDeviceName);
    expectedResolution =
        installErrorMapping[InstallErrorCodes::InvalidToken].second;

    outputMessage = updateDebugToken->getMessage(testOperationType,
                                                 testErrorCode, testDeviceName);
    EXPECT_EQ(expectedMessageStatus, outputMessage.has_value());
    if (outputMessage)
    {
        EXPECT_EQ(expectedMessageError, std::get<0>(*outputMessage));
        EXPECT_EQ(expectedResolution, std::get<1>(*outputMessage));
    }

    // Token Erase
    testOperationType = OperationType::TokenErase;
    testErrorCode = static_cast<int>(EraseErrorCodes::EraseInternalError);
    expectedMessageError =
        eraseErrorMapping[EraseErrorCodes::EraseInternalError].first;
    expectedMessageError =
        updateDebugToken->formatMessage(expectedMessageError, testDeviceName);
    expectedResolution =
        eraseErrorMapping[EraseErrorCodes::EraseInternalError].second;

    outputMessage = updateDebugToken->getMessage(testOperationType,
                                                 testErrorCode, testDeviceName);
    EXPECT_EQ(expectedMessageStatus, outputMessage.has_value());
    if (outputMessage)
    {
        EXPECT_EQ(expectedMessageError, std::get<0>(*outputMessage));
        EXPECT_EQ(expectedResolution, std::get<1>(*outputMessage));
    }

    // Background copy
    testOperationType = OperationType::BackgroundCopy;
    testErrorCode =
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundEnableFail);
    expectedMessageError = backgroundCopyErrorMapping
                               [BackgroundCopyErrorCodes::BackgroundEnableFail]
                                   .first;
    expectedMessageError =
        updateDebugToken->formatMessage(expectedMessageError, testDeviceName);
    expectedResolution = backgroundCopyErrorMapping
                             [BackgroundCopyErrorCodes::BackgroundEnableFail]
                                 .second;

    outputMessage = updateDebugToken->getMessage(testOperationType,
                                                 testErrorCode, testDeviceName);
    EXPECT_EQ(expectedMessageStatus, outputMessage.has_value());
    if (outputMessage)
    {
        EXPECT_EQ(expectedMessageError, std::get<0>(*outputMessage));
        EXPECT_EQ(expectedResolution, std::get<1>(*outputMessage));
    }

    // Common error codes
    testOperationType = OperationType::Common;
    testErrorCode = static_cast<int>(CommonErrorCodes::TokenParseFailure);
    expectedMessageError =
        debugTokenCommonErrorMapping[CommonErrorCodes::TokenParseFailure].first;
    expectedMessageError =
        updateDebugToken->formatMessage(expectedMessageError, testDeviceName);
    expectedResolution =
        debugTokenCommonErrorMapping[CommonErrorCodes::TokenParseFailure]
            .second;

    outputMessage = updateDebugToken->getMessage(testOperationType,
                                                 testErrorCode, testDeviceName);
    EXPECT_EQ(expectedMessageStatus, outputMessage.has_value());
    if (outputMessage)
    {
        EXPECT_EQ(expectedMessageError, std::get<0>(*outputMessage));
        EXPECT_EQ(expectedResolution, std::get<1>(*outputMessage));
    }
}
