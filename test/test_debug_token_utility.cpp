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

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

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

static std::vector<uint8_t> makeRawItem(uint16_t type,
                                        const std::vector<uint8_t>& data)
{
    debug_token::ItemHeader hdr{};
    hdr.type = htole16(type);
    hdr.size = htole16(static_cast<uint16_t>(data.size()));
    std::vector<uint8_t> result(sizeof(debug_token::ItemHeader) + data.size());
    std::memcpy(result.data(), &hdr, sizeof(debug_token::ItemHeader));
    std::memcpy(result.data() + sizeof(debug_token::ItemHeader), data.data(),
                data.size());
    return result;
}

static std::vector<uint8_t>
    makeRawStructure(uint16_t vMajor, uint16_t vMinor,
                     const std::vector<std::vector<uint8_t>>& rawItems)
{
    size_t payloadSize = 0;
    for (const auto& item : rawItems)
    {
        payloadSize += item.size();
    }

    debug_token::StructureHeader hdr{};
    std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(vMajor);
    hdr.versionMinor = htole16(vMinor);
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));

    std::vector<uint8_t> result(sizeof(debug_token::StructureHeader) +
                                payloadSize);
    std::memcpy(result.data(), &hdr, sizeof(debug_token::StructureHeader));
    size_t offset = sizeof(debug_token::StructureHeader);
    for (const auto& item : rawItems)
    {
        std::memcpy(result.data() + offset, item.data(), item.size());
        offset += item.size();
    }
    return result;
}

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
    // Create a valid TLV structure with DeviceType item but NO
    // DeviceSerialNumber. extractSerialNumberFromTlv should return "".

    // Build an item: type=0x0001 (DeviceType), size=1, data=0x01
    debug_token::ItemHeader itemHdr;
    itemHdr.type = htole16(0x0001);
    itemHdr.size = htole16(1);
    uint8_t itemData = 0x01;
    size_t itemTotalSize = sizeof(debug_token::ItemHeader) + 1;

    // Build structure header: identifier="TLV1", payload = item
    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(static_cast<uint32_t>(itemTotalSize));

    std::vector<uint8_t> tlvData(sizeof(debug_token::StructureHeader) +
                                 itemTotalSize);
    std::memcpy(tlvData.data(), &structHdr, sizeof(structHdr));
    std::memcpy(tlvData.data() + sizeof(structHdr), &itemHdr, sizeof(itemHdr));
    tlvData[sizeof(structHdr) + sizeof(itemHdr)] = itemData;

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

TEST_F(TestDebugTokenUtility, ExtractSerialNumberFromTlvValidItem)
{
    auto serialItem =
        makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                    {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67});
    auto typeItem = makeRawItem(debug_token::types::Common::DeviceType, {0x01});
    auto encoded = makeRawStructure(2, 0, {typeItem, serialItem});

    debug_token::tlv_decoder::Structure tlvStructure(encoded);
    EXPECT_EQ(TokenUtility::extractSerialNumberFromTlv(tlvStructure),
              "0xDEADBEEF01234567");
}

TEST_F(TestDebugTokenUtility, ExtractSerialNumberFromTlvEmptySerial)
{
    auto serialItem =
        makeRawItem(debug_token::types::Common::DeviceSerialNumber, {});
    auto encoded = makeRawStructure(2, 0, {serialItem});

    debug_token::tlv_decoder::Structure tlvStructure(encoded);
    EXPECT_EQ(TokenUtility::extractSerialNumberFromTlv(tlvStructure), "0x");
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

TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordTruncatedPayload)
{
    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(8);

    std::vector<uint8_t> tokenData(sizeof(debug_token::StructureHeader) + 4, 0);
    std::memcpy(tokenData.data(), &structHdr, sizeof(structHdr));

    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_EQ(
        TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens, recordSize),
        -1);
    EXPECT_EQ(recordSize, sizeof(debug_token::StructureHeader) + 8);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordMissingSerialSkipsRecord)
{
    debug_token::ItemHeader itemHdr{};
    itemHdr.type = htole16(0x0001);
    itemHdr.size = htole16(1);

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size =
        htole32(sizeof(debug_token::ItemHeader) + static_cast<uint32_t>(1));

    std::vector<uint8_t> tokenData(sizeof(debug_token::StructureHeader) +
                                       sizeof(debug_token::ItemHeader) + 1,
                                   0);
    std::memcpy(tokenData.data(), &structHdr, sizeof(structHdr));
    std::memcpy(tokenData.data() + sizeof(structHdr), &itemHdr,
                sizeof(itemHdr));
    tokenData.back() = 0x01;

    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_EQ(
        TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens, recordSize),
        0);
    EXPECT_EQ(recordSize, sizeof(debug_token::StructureHeader) +
                              sizeof(debug_token::ItemHeader) + 1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordValidRecordStoresToken)
{
    debug_token::ItemHeader itemHdr{};
    itemHdr.type = htole16(0x0003);
    itemHdr.size = htole16(8);
    std::array<uint8_t, 8> serial = {0x40, 0x41, 0x42, 0x43,
                                     0x44, 0x45, 0x46, 0x47};

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(sizeof(debug_token::ItemHeader) + serial.size());

    std::vector<uint8_t> tokenData(sizeof(debug_token::StructureHeader) +
                                       sizeof(debug_token::ItemHeader) +
                                       serial.size(),
                                   0);
    std::memcpy(tokenData.data(), &structHdr, sizeof(structHdr));
    std::memcpy(tokenData.data() + sizeof(structHdr), &itemHdr,
                sizeof(itemHdr));
    std::memcpy(tokenData.data() + sizeof(structHdr) + sizeof(itemHdr),
                serial.data(), serial.size());

    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_EQ(
        TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens, recordSize),
        0);
    EXPECT_EQ(recordSize, tokenData.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x4041424344454647"));
}

TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordValidRecordAtNonZeroOffset)
{
    debug_token::ItemHeader itemHdr{};
    itemHdr.type = htole16(0x0003);
    itemHdr.size = htole16(8);
    std::array<uint8_t, 8> serial = {0x60, 0x61, 0x62, 0x63,
                                     0x64, 0x65, 0x66, 0x67};

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(sizeof(debug_token::ItemHeader) + serial.size());

    std::vector<uint8_t> record(sizeof(debug_token::StructureHeader) +
                                    sizeof(debug_token::ItemHeader) +
                                    serial.size(),
                                0);
    std::memcpy(record.data(), &structHdr, sizeof(structHdr));
    std::memcpy(record.data() + sizeof(structHdr), &itemHdr, sizeof(itemHdr));
    std::memcpy(record.data() + sizeof(structHdr) + sizeof(itemHdr),
                serial.data(), serial.size());

    std::vector<uint8_t> tokenData = {0xAA, 0xBB, 0xCC};
    tokenData.insert(tokenData.end(), record.begin(), record.end());
    tokenData.push_back(0xDD);

    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_EQ(
        TokenUtility::parseSingleTlvRecord(tokenData, 3, tokens, recordSize),
        0);
    EXPECT_EQ(recordSize, record.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x6061626364656667"));
}

TEST_F(TestDebugTokenUtility, ParseSingleTlvRecordOffsetNearEndFails)
{
    std::vector<uint8_t> tokenData = {0xAA, 0xBB, 0xCC, 0xDD};
    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_EQ(TokenUtility::parseSingleTlvRecord(
                  tokenData, tokenData.size() - 1, tokens, recordSize),
              -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestDebugTokenUtility, ParseTlvTokensHandlesMalformedRecord)
{
    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 1;
    header.offsetToListOfStructs = 16;

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "BAD!", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(0);

    std::vector<uint8_t> fullFile(
        header.offsetToListOfStructs + sizeof(debug_token::StructureHeader), 0);
    std::memcpy(fullFile.data() + header.offsetToListOfStructs, &structHdr,
                sizeof(structHdr));

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestDebugTokenUtility, ParseTlvTokensParsesMultipleRecords)
{
    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);

    debug_token::ItemHeader itemOne{};
    itemOne.type = htole16(0x0003);
    itemOne.size = htole16(8);
    std::array<uint8_t, 8> serialOne = {0x10, 0x11, 0x12, 0x13,
                                        0x14, 0x15, 0x16, 0x17};

    debug_token::StructureHeader structOne{};
    std::memcpy(structOne.identifier, "TLV1", 4);
    structOne.versionMajor = htole16(2);
    structOne.versionMinor = htole16(0);
    structOne.size = htole32(sizeof(debug_token::ItemHeader) + 8);

    debug_token::ItemHeader itemTwo{};
    itemTwo.type = htole16(0x0003);
    itemTwo.size = htole16(8);
    std::array<uint8_t, 8> serialTwo = {0x20, 0x21, 0x22, 0x23,
                                        0x24, 0x25, 0x26, 0x27};

    debug_token::StructureHeader structTwo{};
    std::memcpy(structTwo.identifier, "TLV1", 4);
    structTwo.versionMajor = htole16(2);
    structTwo.versionMinor = htole16(0);
    structTwo.size = htole32(sizeof(debug_token::ItemHeader) + 8);

    std::vector<uint8_t> fullFile(
        sizeof(DebugTokenHeader) + 2 * (sizeof(debug_token::StructureHeader) +
                                        sizeof(debug_token::ItemHeader) + 8),
        0);
    size_t offset = 0;
    std::memcpy(fullFile.data(), &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fullFile.data() + offset, &structOne, sizeof(structOne));
    offset += sizeof(structOne);
    std::memcpy(fullFile.data() + offset, &itemOne, sizeof(itemOne));
    offset += sizeof(itemOne);
    std::memcpy(fullFile.data() + offset, serialOne.data(), serialOne.size());
    offset += serialOne.size();
    std::memcpy(fullFile.data() + offset, &structTwo, sizeof(structTwo));
    offset += sizeof(structTwo);
    std::memcpy(fullFile.data() + offset, &itemTwo, sizeof(itemTwo));
    offset += sizeof(itemTwo);
    std::memcpy(fullFile.data() + offset, serialTwo.data(), serialTwo.size());

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), 0);
    EXPECT_EQ(tokens.size(), 2u);
    EXPECT_TRUE(tokens.contains("0x1011121314151617"));
    EXPECT_TRUE(tokens.contains("0x2021222324252627"));
}

TEST_F(TestDebugTokenUtility, ParseTlvTokensContinuesPastMalformedRecord)
{
    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);

    debug_token::StructureHeader malformed{};
    std::memcpy(malformed.identifier, "BAD!", 4);
    malformed.versionMajor = htole16(2);
    malformed.versionMinor = htole16(0);
    malformed.size = htole32(0);

    debug_token::ItemHeader validItem{};
    validItem.type = htole16(0x0003);
    validItem.size = htole16(8);
    std::array<uint8_t, 8> serial = {0x30, 0x31, 0x32, 0x33,
                                     0x34, 0x35, 0x36, 0x37};

    debug_token::StructureHeader validStruct{};
    std::memcpy(validStruct.identifier, "TLV1", 4);
    validStruct.versionMajor = htole16(2);
    validStruct.versionMinor = htole16(0);
    validStruct.size = htole32(sizeof(debug_token::ItemHeader) + 8);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) +
                                      sizeof(debug_token::StructureHeader) +
                                      sizeof(debug_token::StructureHeader) +
                                      sizeof(debug_token::ItemHeader) + 8,
                                  0);
    size_t offset = 0;
    std::memcpy(fullFile.data(), &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fullFile.data() + offset, &malformed, sizeof(malformed));
    offset += sizeof(malformed);
    std::memcpy(fullFile.data() + offset, &validStruct, sizeof(validStruct));
    offset += sizeof(validStruct);
    std::memcpy(fullFile.data() + offset, &validItem, sizeof(validItem));
    offset += sizeof(validItem);
    std::memcpy(fullFile.data() + offset, serial.data(), serial.size());

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), -1);
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x3031323334353637"));
}

TEST_F(TestDebugTokenUtility, ParseTlvTokensSkipsMissingSerialBeforeValidRecord)
{
    auto missingSerialRecord = makeRawStructure(
        2, 0, {makeRawItem(debug_token::types::Common::DeviceType, {0x01})});
    auto validSerialRecord = makeRawStructure(
        2, 0,
        {makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                     {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7})});

    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) +
                                      missingSerialRecord.size() +
                                      validSerialRecord.size(),
                                  0);
    size_t offset = 0;
    std::memcpy(fullFile.data(), &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fullFile.data() + offset, missingSerialRecord.data(),
                missingSerialRecord.size());
    offset += missingSerialRecord.size();
    std::memcpy(fullFile.data() + offset, validSerialRecord.data(),
                validSerialRecord.size());

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), 0);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0xA0A1A2A3A4A5A6A7"));
}

TEST_F(TestDebugTokenUtility, ParseTlvTokensDuplicateSerialKeepsFirstRecord)
{
    auto firstRecord = makeRawStructure(
        2, 0,
        {makeRawItem(debug_token::types::Common::DeviceType, {0x01}),
         makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                     {0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C})});
    auto secondRecord = makeRawStructure(
        2, 0,
        {makeRawItem(debug_token::types::Common::DeviceType, {0x02}),
         makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                     {0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C})});

    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(
        sizeof(DebugTokenHeader) + firstRecord.size() + secondRecord.size(), 0);
    size_t offset = 0;
    std::memcpy(fullFile.data(), &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fullFile.data() + offset, firstRecord.data(),
                firstRecord.size());
    offset += firstRecord.size();
    std::memcpy(fullFile.data() + offset, secondRecord.data(),
                secondRecord.size());

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), 0);
    ASSERT_EQ(tokens.size(), 1u);
    auto it = tokens.find("0x55565758595A5B5C");
    ASSERT_NE(it, tokens.end());
    EXPECT_EQ(it->second, firstRecord);
}

TEST_F(TestDebugTokenUtility, CreateTokenInstallErrorMessageDoesNotThrow)
{
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken updater(bus);
    EXPECT_NO_THROW(updater.createTokenInstallErrorMessage("/xyz/device0"));
}

TEST_F(TestDebugTokenUtility, CreateTokenEraseErrorMessageDoesNotThrow)
{
    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken updater(bus);
    EXPECT_NO_THROW(updater.createTokenEraseErrorMessage("/xyz/device0"));
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

TEST_F(TestDebugTokenUtility, ParseTlvTokensStopsWhenOffsetReachesEnd)
{
    DebugTokenHeader header{};
    header.version = 2;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);

    debug_token::ItemHeader itemHdr{};
    itemHdr.type = htole16(0x0003);
    itemHdr.size = htole16(8);
    std::array<uint8_t, 8> serial = {0x50, 0x51, 0x52, 0x53,
                                     0x54, 0x55, 0x56, 0x57};

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(sizeof(debug_token::ItemHeader) + serial.size());

    std::vector<uint8_t> fullFile(
        sizeof(DebugTokenHeader) + sizeof(debug_token::StructureHeader) +
            sizeof(debug_token::ItemHeader) + serial.size(),
        0);
    size_t offset = 0;
    std::memcpy(fullFile.data(), &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fullFile.data() + offset, &structHdr, sizeof(structHdr));
    offset += sizeof(structHdr);
    std::memcpy(fullFile.data() + offset, &itemHdr, sizeof(itemHdr));
    offset += sizeof(itemHdr);
    std::memcpy(fullFile.data() + offset, serial.data(), serial.size());

    TokenMap tokens;
    EXPECT_EQ(TokenUtility::parseTlvTokens(fullFile, &header, tokens), -1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x5051525354555657"));
}

TEST_F(TestDebugTokenUtility, GetNextDebugTokenReadsSecondTokenAtOffset)
{
    constexpr uint16_t firstTokenSize = 80;
    constexpr uint16_t secondTokenSize = 96;
    std::string tmpPath = "/tmp/test_second_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);

        std::vector<uint8_t> firstToken(firstTokenSize, 0x11);
        TokenHeader firstHdr{};
        std::memcpy(firstHdr.identifier, "EDTI", 4);
        firstHdr.structSize = firstTokenSize;
        std::memcpy(firstToken.data(), &firstHdr, sizeof(firstHdr));

        std::vector<uint8_t> secondToken(secondTokenSize, 0x22);
        TokenHeader secondHdr{};
        std::memcpy(secondHdr.identifier, "MCDT", 4);
        secondHdr.structSize = secondTokenSize;
        std::memcpy(secondToken.data(), &secondHdr, sizeof(secondHdr));

        f.write(reinterpret_cast<const char*>(firstToken.data()),
                firstToken.size());
        f.write(reinterpret_cast<const char*>(secondToken.data()),
                secondToken.size());
    }

    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, firstTokenSize, tokenData, serialNumber);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(tokenData.size(), secondTokenSize);
    EXPECT_EQ(serialNumber.size(), 16u);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestDebugTokenUtility, GetNextDebugTokenMcuTruncatedBody)
{
    std::string tmpPath = "/tmp/test_mcu_truncated_body.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "MCDT", 4);
        tokenHdr.structSize = sizeof(TokenHeader) + 16 + 32;
        f.write(reinterpret_cast<const char*>(&tokenHdr), sizeof(tokenHdr));
        std::vector<uint8_t> serial(16, 0x33);
        f.write(reinterpret_cast<const char*>(serial.data()), serial.size());
    }

    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, 0, tokenData, serialNumber);
    EXPECT_EQ(hdr, nullptr);
    EXPECT_TRUE(tokenData.empty());
    EXPECT_EQ(serialNumber.size(), 16u);
    std::filesystem::remove(tmpPath);
}
