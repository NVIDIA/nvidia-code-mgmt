/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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

#include "../debug_token/tlv/tlv.h"

#include "../debug_token/token_utility.hpp"
#include "../debug_token/update_debug_token.hpp"

#include <stdlib.h>

#include <cstdint>
#include <cstring>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

class TestDebugTokenUtility : public testing::Test, public TokenUtility
{
  public:
    TestDebugTokenUtility()
    {}

    ~TestDebugTokenUtility()
    {}
};

TEST_F(TestDebugTokenUtility, DebugTokenEraseResponse)
{
    std::string cmdResponse = "Test command = debug_token_erase\n"
                              "teid = 24\n"
                              "TX: 47 16 00 00 80 01 0C 01\n"
                              "RX: 47 16 00 00 00 01 0C 01 00 00";
    auto rxBytes = parseCommandOutput(cmdResponse);
    // last byte is status code
    auto status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    EXPECT_EQ(status, 0);
}

TEST_F(TestDebugTokenUtility, DebugTokenInstallResponse)
{
    std::string cmdResponse =
        "Test command = debug_token_install\n"
        "teid = 24\n"
        "length 256\n"
        "TX: 47 16 00 00 80 01 0B 01 45 44 54 49 01 00 00 00 00 01 02 00 01 00"
        " 00 00 46 01 00 00 2C 3D 17 56 CA 72 86 1A 0F C6 5D 0A 78 27 6C EB 01"
        " 1E 02 0E 16 0A 10 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
        " 00 00 00 00 87 E9 F7 C7 58 7B 7E 9B 71 BC FE C3 2B 1B 17 04 11 C6 CF"
        " C3 D9 66 8A A9 60 A1 9D 82 7A 49 06 03 0D CD 95 7C E7 53 EC 3B 0C 14"
        " 56 41 87 AF 2D 69 28 83 60 E8 A8 3A BD E9 45 48 D0 2E A0 A6 54 A8 F4"
        " C0 A8 78 DC CD D7 A0 7E CC 82 AF CE 59 4E 8E 36 BB 1D 31 D8 D8 C7 6D"
        " 07 22 8A EB A3 22 62 84 A7 7A ED A2 08 1A 5C CB AF 94 A1 55 23 F7 8A"
        " 0D 72 BF 29 C0 F5 B4 D8 AE 1F 52 41 F4 11 8F A3 D5 A1 B6 A2 10 B6 71"
        " 11 3F FD 72 19 BA F9 5C 6D 2F 85 7C EC CF AF DA BA A4 AF C5 BC 13 98"
        " 9F C4 4A 52 A4 F2 DC 85 34 72 4A 41 5F 57 2E AA 6A 9F DE EF BF 3F F2"
        " 7C 78 65 50 5B 98 80 55 12 AC 9F 43\n"
        "RX: 47 16 00 00 00 01 0B 01 01 0";
    auto rxBytes = parseCommandOutput(cmdResponse);
    // last byte is status code
    auto status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    EXPECT_EQ(status, 0);
}

TEST_F(TestDebugTokenUtility, DebugTokenQueryResponse)
{
    std::string cmdResponse =
        "Test command = debug_token_query\n"
        "teid = 25\n"
        "TX: 47 16 00 00 80 01 0F 01\n"
        "RX: 47 16 00 00 00 01 0F 01 00 00 02 1E 05 06 16 0B 04 01 01";
    auto rxBytes = parseCommandOutput(cmdResponse);
    // 11 the byte from last is status code
    auto status = std::stoi(rxBytes[rxBytes.size() - 11], nullptr, 16);
    EXPECT_EQ(status, 0);
    // 10 the byte from last is token installation status
    auto tokenInstallStatus =
        std::stoi(rxBytes[rxBytes.size() - 10], nullptr, 16);
    EXPECT_EQ(tokenInstallStatus, 0);
}
// Test formatSerialNumber utility function
TEST_F(TestDebugTokenUtility, FormatSerialNumberValidInput)
{
    std::vector<uint8_t> serialBytes = {0x12, 0x34, 0xAB, 0xCD,
                                        0xEF, 0x56, 0x78, 0x90};
    std::string result = TokenUtility::formatSerialNumber(serialBytes);
    EXPECT_EQ(result, "0x1234ABCDEF567890");
}
TEST_F(TestDebugTokenUtility, FormatSerialNumberEmptyInput)
{
    std::vector<uint8_t> serialBytes = {};
    std::string result = TokenUtility::formatSerialNumber(serialBytes);
    EXPECT_EQ(result, "0x");
}
TEST_F(TestDebugTokenUtility, FormatSerialNumberSingleByte)
{
    std::vector<uint8_t> serialBytes = {0x0F};
    std::string result = TokenUtility::formatSerialNumber(serialBytes);
    EXPECT_EQ(result, "0x0F");
}
TEST_F(TestDebugTokenUtility, FormatSerialNumberLeadingZeros)
{
    std::vector<uint8_t> serialBytes = {0x00, 0x01, 0x0A, 0x0B};
    std::string result = TokenUtility::formatSerialNumber(serialBytes);
    EXPECT_EQ(result, "0x00010A0B");
}
// Test extractSerialNumberFromTlv with missing serial number
TEST_F(TestDebugTokenUtility, ExtractSerialNumberFromTlvMissingItem)
{
    // Create minimal TLV structure without serial number
    std::vector<uint8_t> tlvData(32 + 4, 0);
    std::memcpy(tlvData.data(), "TLV1", 4);
    uint16_t version = htole16(2);
    std::memcpy(tlvData.data() + 4, &version, 2);
    std::memcpy(tlvData.data() + 6, &version, 2);
    uint32_t size = htole32(0);
    std::memcpy(tlvData.data() + 8, &size, 4);
    try
    {
        debug_token::tlv_decoder::Structure tlvStructure(tlvData);
        std::string result =
            TokenUtility::extractSerialNumberFromTlv(tlvStructure);
        EXPECT_EQ(result, "");
    }
    catch (const std::exception&)
    {
        FAIL() << "Should not throw exception for missing serial number";
    }
}
// Test parseSingleTlvRecord with insufficient data
TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordInsufficientData)
{
    std::vector<uint8_t> tokenData = {0x01, 0x02, 0x03};
    TokenMap tokens;
    size_t recordSize = 0;
    int result =
        TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens, recordSize);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}
// Test parseTlvTokens with empty token file
TEST_F(TestDebugTokenUtility, ParseTlvTokensNoValidTokens)
{
    DebugTokenHeader header;
    header.version = 2;
    header.numberOfRecords = 0;
    header.offsetToListOfStructs = 16;
    std::vector<uint8_t> fullFile(16, 0);
    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &header, tokens);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}