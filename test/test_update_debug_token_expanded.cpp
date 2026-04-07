/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

#include <endian.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>

namespace alloc_fail
{
thread_local bool enabled = false;
thread_local size_t failAt = 0;
thread_local size_t allocationCount = 0;

void reset()
{
    enabled = false;
    failAt = 0;
    allocationCount = 0;
}

void* allocate(std::size_t size)
{
    if (enabled && failAt != 0 && ++allocationCount == failAt)
    {
        throw std::bad_alloc();
    }

    if (void* ptr = std::malloc(size == 0 ? 1 : size))
    {
        return ptr;
    }
    throw std::bad_alloc();
}

class Guard
{
  public:
    explicit Guard(size_t nthAllocation)
    {
        enabled = true;
        failAt = nthAllocation;
        allocationCount = 0;
    }

    ~Guard()
    {
        reset();
    }
};
} // namespace alloc_fail

void* operator new(std::size_t size)
{
    return alloc_fail::allocate(size);
}

void* operator new[](std::size_t size)
{
    return alloc_fail::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return alloc_fail::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return alloc_fail::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

template <typename Callback>
void swallowAll(Callback&& callback)
{
    try
    {
        callback();
    }
    catch (const std::bad_alloc&)
    {}
    catch (const std::exception&)
    {}
}

template <typename PrepareFn, typename InvokeFn>
size_t countAllocations(PrepareFn&& prepare, InvokeFn&& invoke)
{
    prepare();
    alloc_fail::enabled = true;
    alloc_fail::failAt = std::numeric_limits<size_t>::max();
    alloc_fail::allocationCount = 0;
    swallowAll(std::forward<InvokeFn>(invoke));
    size_t totalAllocations = alloc_fail::allocationCount;
    alloc_fail::reset();
    return totalAllocations;
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredAllocFailureSweep(PrepareFn&& prepare, InvokeFn&& invoke)
{
    const size_t totalAllocations = countAllocations(
        std::forward<PrepareFn>(prepare), std::forward<InvokeFn>(invoke));
    if (totalAllocations == 0)
    {
        return;
    }

    for (size_t failIndex = 1; failIndex <= totalAllocations; ++failIndex)
    {
        prepare();
        alloc_fail::Guard guard(failIndex);
        swallowAll(invoke);
    }
}

class TestUpdateDebugTokenExpanded : public testing::Test
{
  public:
    std::unique_ptr<UpdateDebugToken> updateDebugToken;
    TestUpdateDebugTokenExpanded()
    {
        auto bus = sdbusplus::bus::new_default();
        updateDebugToken = std::make_unique<UpdateDebugToken>(bus);
    }
    ~TestUpdateDebugTokenExpanded()
    {}

    void SetUp() override
    {
        alloc_fail::reset();
    }
};

// ========================== formatMessage Tests ==========================

TEST_F(TestUpdateDebugTokenExpanded, FormatMessageEmptyDeviceName)
{
    std::string msg = "Error occurred for {}.";
    auto result = updateDebugToken->formatMessage(msg, "");
    EXPECT_EQ(result, msg); // Returns unformatted when device name empty
}

TEST_F(TestUpdateDebugTokenExpanded, FormatMessageWithDeviceName)
{
    std::string msg = "Error for {}.";
    auto result = updateDebugToken->formatMessage(msg, "GPU_0");
    EXPECT_EQ(result, "Error for GPU_0.");
}

TEST_F(TestUpdateDebugTokenExpanded, FormatMessageNoPlaceholder)
{
    std::string msg = "Simple message with no placeholder.";
    auto result = updateDebugToken->formatMessage(msg, "GPU_0");
    EXPECT_EQ(result, "Simple message with no placeholder.");
}

// ========================== getMessage Tests ==========================

// TokenInstall: known error codes
TEST_F(TestUpdateDebugTokenExpanded, GetMessageTokenInstallAllCodes)
{
    std::string device = "ERoT_GPU_0";
    auto codes = {
        InstallErrorCodes::InvalidToken,
        InstallErrorCodes::TokenAuthFailed,
        InstallErrorCodes::TokenNonceInvalid,
        InstallErrorCodes::TokenSerialNumberInvalid,
        InstallErrorCodes::TokenECFWVersionInvalid,
        InstallErrorCodes::DisableBackgroundCopyCheckFailed,
        InstallErrorCodes::InstallInternalError,
    };
    for (auto code : codes)
    {
        auto msg = updateDebugToken->getMessage(OperationType::TokenInstall,
                                                static_cast<int>(code), device);
        EXPECT_TRUE(msg.has_value())
            << "Missing message for install code " << static_cast<int>(code);
    }
}

// TokenInstall: unknown code returns nullopt
TEST_F(TestUpdateDebugTokenExpanded, GetMessageTokenInstallUnknownCode)
{
    auto msg = updateDebugToken->getMessage(OperationType::TokenInstall, 999,
                                            "device");
    EXPECT_FALSE(msg.has_value());
}

// TokenErase: all known codes
TEST_F(TestUpdateDebugTokenExpanded, GetMessageTokenEraseAllCodes)
{
    auto codes = {
        EraseErrorCodes::EraseInternalError,
        EraseErrorCodes::EraseFailed,
    };
    for (auto code : codes)
    {
        auto msg = updateDebugToken->getMessage(
            OperationType::TokenErase, static_cast<int>(code), "device");
        EXPECT_TRUE(msg.has_value())
            << "Missing message for erase code " << static_cast<int>(code);
    }
}

// TokenErase: unknown code
TEST_F(TestUpdateDebugTokenExpanded, GetMessageTokenEraseUnknownCode)
{
    auto msg =
        updateDebugToken->getMessage(OperationType::TokenErase, 999, "device");
    EXPECT_FALSE(msg.has_value());
}

// BackgroundCopy: all known codes
TEST_F(TestUpdateDebugTokenExpanded, GetMessageBackgroundCopyAllCodes)
{
    auto codes = {
        BackgroundCopyErrorCodes::BackgroundEnableFail,
        BackgroundCopyErrorCodes::BackgroundDisableFail,
    };
    for (auto code : codes)
    {
        auto msg = updateDebugToken->getMessage(
            OperationType::BackgroundCopy, static_cast<int>(code), "device");
        EXPECT_TRUE(msg.has_value())
            << "Missing message for bg copy code " << static_cast<int>(code);
    }
}

// BackgroundCopy: unknown code
TEST_F(TestUpdateDebugTokenExpanded, GetMessageBackgroundCopyUnknownCode)
{
    auto msg = updateDebugToken->getMessage(OperationType::BackgroundCopy, 999,
                                            "device");
    EXPECT_FALSE(msg.has_value());
}

// Common: all known codes
TEST_F(TestUpdateDebugTokenExpanded, GetMessageCommonAllCodes)
{
    auto codes = {
        CommonErrorCodes::MCTPDiscoveryFailed,
        CommonErrorCodes::TokenParseFailure,
        CommonErrorCodes::MCTPCommandInstallSuccess,
        CommonErrorCodes::MCTPCommandInstallFailure,
        CommonErrorCodes::MCTPCommandEraseSuccess,
        CommonErrorCodes::MCTPCommandEraseFailure,
        CommonErrorCodes::MCTPResponseInstallFailure,
        CommonErrorCodes::MCTPResponseEraseFailure,
        CommonErrorCodes::NSMCommandInstallSuccess,
        CommonErrorCodes::NSMCommandInstallFailure,
        CommonErrorCodes::NSMCommandEraseSuccess,
        CommonErrorCodes::NSMCommandEraseFailure,
        CommonErrorCodes::NSMCommandFailure,
    };
    for (auto code : codes)
    {
        auto msg = updateDebugToken->getMessage(
            OperationType::Common, static_cast<int>(code), "device");
        EXPECT_TRUE(msg.has_value())
            << "Missing message for common code " << static_cast<int>(code);
    }
}

// Common: unknown code
TEST_F(TestUpdateDebugTokenExpanded, GetMessageCommonUnknownCode)
{
    auto msg =
        updateDebugToken->getMessage(OperationType::Common, 999, "device");
    EXPECT_FALSE(msg.has_value());
}

// TokenQueryStatus: unknown operation type (uses default branch)
TEST_F(TestUpdateDebugTokenExpanded, GetMessageQueryStatusUnknownOp)
{
    auto msg = updateDebugToken->getMessage(OperationType::TokenQueryStatus, 0,
                                            "device");
    EXPECT_FALSE(msg.has_value());
}

TEST_F(TestUpdateDebugTokenExpanded, FormatAndGetMessageAllocationFailureSweep)
{
    auto prepare = [&] { alloc_fail::reset(); };
    auto invoke = [&] {
        (void)updateDebugToken->formatMessage("Error for {}.", "GPU_0");
        (void)updateDebugToken->formatMessage("Simple message.", "GPU_1");
        (void)updateDebugToken->formatMessage("{}", "");
        (void)updateDebugToken->getMessage(
            OperationType::TokenInstall,
            static_cast<int>(InstallErrorCodes::InstallInternalError),
            std::string(96, 'I'));
        (void)updateDebugToken->getMessage(
            OperationType::TokenErase,
            static_cast<int>(EraseErrorCodes::EraseInternalError),
            std::string(96, 'E'));
        (void)updateDebugToken->getMessage(
            OperationType::BackgroundCopy,
            static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopyFailed),
            std::string(96, 'B'));
        (void)updateDebugToken->getMessage(
            OperationType::Common,
            static_cast<int>(CommonErrorCodes::MCTPCommandInstallFailure),
            std::string(96, 'C'));
        (void)updateDebugToken->getMessage(OperationType::TokenInstall, 999,
                                           std::string(96, 'U'));
        (void)updateDebugToken->getMessage(OperationType::TokenErase, 999,
                                           std::string(96, 'V'));
        (void)updateDebugToken->getMessage(OperationType::BackgroundCopy, 999,
                                           std::string(96, 'W'));
        (void)updateDebugToken->getMessage(OperationType::Common, 999,
                                           std::string(96, 'X'));
        (void)updateDebugToken->getMessage(OperationType::TokenQueryStatus, 0,
                                           std::string(96, 'Q'));
    };

    invoke();
    runMeasuredAllocFailureSweep(prepare, invoke);
}

// ========================== parseQueryV2Response Tests
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2ResponseTooShort)
{
    // Response shorter than completion code byte
    std::vector<std::string> rxBytes = {"00", "00", "16"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2ResponseCompletionCodeError)
{
    // Valid length but non-zero completion code (wrong size for v2)
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "03"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, 3); // completion code = 03
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2SuccessTokenNotInstalled)
{
    // Full v2 response (37 bytes), token not installed
    std::vector<std::string> rxBytes = {
        "00", "00", "16", "47", "00", "01", "0F", "02", // header
        "00",                                           // completion=0
        "00", // token status=not installed
        "43", "37", "66", "B5", "22", "FD", "E2", "BF", // nonce
        "02",                                           // version
        "00", "00", "00",                               // reserved
        "00", "10", "00", "14",                         // caps
        "00", "00", "00", "00",                         // type bytes
        "00", "00", "00", "00",                         // more type bytes
        "00", "00", "00"                                // padding
    };
    int tokenInstallStatus = 0, installedTokenType = 0;
    updateDebugToken->parseQueryV2Response(rxBytes, tokenInstallStatus,
                                           installedTokenType, 31);
    EXPECT_EQ(tokenInstallStatus, 0);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2SuccessTokenInstalled)
{
    // Full v2 response (37 bytes), token installed with type=1
    std::vector<std::string> rxBytes = {
        "00", "00", "16", "47", "00", "01", "0F", "02", // header
        "00",                                           // completion=0
        "01", // token status=installed
        "43", "37", "66", "B5", "22", "FD", "E2", "BF", // nonce
        "02",                                           // version
        "01", "00", "00", // installed token type byte 19=1
        "00", "10", "00", "14", "00", "00", "00", "00",
        "00", "00", "00", "00", "00", "00", "00"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    updateDebugToken->parseQueryV2Response(rxBytes, tokenInstallStatus,
                                           installedTokenType, 31);
    EXPECT_EQ(tokenInstallStatus, 1);
    EXPECT_EQ(installedTokenType, 1);
}

// ========================== updateTokenMap Tests ==========================

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapFileNotFound)
{
    TokenMap tokens;
    int result =
        updateDebugToken->updateTokenMap("/nonexistent/path/token.bin", tokens);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapPathIsDirectory)
{
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap("/tmp", tokens);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapInvalidHeader)
{
    // Create a temp file with invalid header (type != 2)
    std::string tmpPath = "/tmp/test_invalid_header.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = 99; // Invalid type
        hdr.numberOfRecords = 0;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);
    EXPECT_EQ(result, -1);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapValidLegacyToken)
{
    // Create a minimal legacy token file (version 1, type 2)
    std::string tmpPath = "/tmp/test_legacy_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);

        // Build a simple token
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.versionMinor = 0;
        tokenHdr.versionMajor = 1;
        tokenHdr.structSize = 256; // Standard token size
        tokenHdr.tokenAttributes = 0;
        tokenHdr.tokenType = 1;
        tokenHdr.ecFWVersion = 0;
        std::memset(tokenHdr.nonce, 0, 16);

        // Serial number (8 bytes for non-MCU)
        std::vector<uint8_t> serial = {0x01, 0x1E, 0x02, 0x0E,
                                       0x16, 0x0A, 0x10, 0x17};

        // Build token data: header + serial + padding to structSize
        std::vector<uint8_t> tokenData(256, 0);
        std::memcpy(tokenData.data(), &tokenHdr, sizeof(TokenHeader));
        std::memcpy(tokenData.data() + sizeof(TokenHeader), serial.data(),
                    serial.size());

        // File header
        DebugTokenHeader fileHdr{};
        fileHdr.version = 1;
        fileHdr.type = 2; // FileTypeDebugToken
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        fileHdr.fileSize = sizeof(DebugTokenHeader) + 256;

        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(tokenData.data()),
                tokenData.size());
    }
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
    // Check serial number key
    EXPECT_TRUE(tokens.contains("0x011E020E160A1017"));
    std::filesystem::remove(tmpPath);
}

// ========================== parseCommandOutput Tests
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, ParseCommandOutputNoRX)
{
    std::string output =
        "Test command = debug_token_query\nteid = 25\nTX: 47 16 00 00 80 01 0F 01\n";
    auto rxBytes = updateDebugToken->parseCommandOutput(output);
    EXPECT_TRUE(rxBytes.empty());
}

TEST_F(TestUpdateDebugTokenExpanded, ParseCommandOutputEmptyString)
{
    auto rxBytes = updateDebugToken->parseCommandOutput("");
    EXPECT_TRUE(rxBytes.empty());
}

TEST_F(TestUpdateDebugTokenExpanded, ParseCommandOutputMultipleRXLines)
{
    // Only last RX line should be captured (overwritten)
    std::string output = "RX: AA BB\nRX: CC DD EE\n";
    auto rxBytes = updateDebugToken->parseCommandOutput(output);
    EXPECT_EQ(rxBytes.size(), 3u);
    EXPECT_EQ(rxBytes[0], "CC");
}

// ========================== MctpEidInfo operator< Tests
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, MctpEidInfoComparisonSameMedium)
{
    MctpEidInfo a{1,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                  {},
                  true};
    MctpEidInfo b{2,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB",
                  {},
                  true};
    // Same medium, different binding - should compare by binding priority
    // PCIe binding (0) vs USB binding (1), so b > a in priority (less means
    // higher)
    EXPECT_TRUE(b < a); // USB binding > PCIe binding in priority number
}

TEST_F(TestUpdateDebugTokenExpanded, MctpEidInfoComparisonDifferentMedium)
{
    MctpEidInfo a{1,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                  {},
                  true};
    MctpEidInfo b{2,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus",
                  {},
                  true};
    // Different medium: PCIe(0) vs SMBus(5) - SMBus has higher number
    EXPECT_TRUE(b < a); // SMBus(5) > PCIe(0)
}

// ========================== TokenUtility functions ==========================

class TestTokenUtilityExpanded : public testing::Test, public TokenUtility
{
  public:
    TestTokenUtilityExpanded()
    {}
    ~TestTokenUtilityExpanded()
    {}
};

TEST_F(TestTokenUtilityExpanded, GetDebugTokenHeaderValidFile)
{
    std::string tmpPath = "/tmp/test_valid_header.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = 2; // FileTypeDebugToken
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
    auto hdr = getDebugTokenHeader(headerData, pkg);
    EXPECT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->type, 2);
    EXPECT_EQ(hdr->version, 1);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, GetDebugTokenHeaderInvalidType)
{
    std::string tmpPath = "/tmp/test_invalid_type.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = 1; // Not FileTypeDebugToken
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
    auto hdr = getDebugTokenHeader(headerData, pkg);
    EXPECT_EQ(hdr, nullptr);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenValid)
{
    std::string tmpPath = "/tmp/test_next_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.structSize = 128;
        std::vector<uint8_t> data(128, 0xAA);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, 0, tokenData, serialNumber);
    EXPECT_NE(hdr, nullptr);
    EXPECT_EQ(tokenData.size(), 128u);
    EXPECT_EQ(serialNumber.size(), 8u); // Default serial size for non-MCU
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenMCU)
{
    std::string tmpPath = "/tmp/test_mcu_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "MCDT", 4); // MCU identifier
        tokenHdr.structSize = 128;
        std::vector<uint8_t> data(128, 0xBB);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, 0, tokenData, serialNumber);
    EXPECT_NE(hdr, nullptr);
    EXPECT_EQ(serialNumber.size(), 16u); // MCU serial size
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenTruncatedHeader)
{
    std::string tmpPath = "/tmp/test_trunc_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        // Write less than sizeof(TokenHeader)
        f.write("AB", 2);
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, 0, tokenData, serialNumber);
    EXPECT_EQ(hdr, nullptr);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, RunMctpVdmUtilCommandEcho)
{
    // Test with a simple command that should succeed
    auto [retCode, output] = runMctpVdmUtilCommand("echo hello");
    EXPECT_EQ(retCode, 0);
    EXPECT_TRUE(output.find("hello") != std::string::npos);
}

TEST_F(TestTokenUtilityExpanded, RunMctpVdmUtilCommandFail)
{
    auto [retCode, output] = runMctpVdmUtilCommand("false");
    EXPECT_NE(retCode, 0);
}

// Helper to build a raw TLV item (type + size + data)
static std::vector<uint8_t> buildRawItem(uint16_t type,
                                         const std::vector<uint8_t>& data)
{
    debug_token::ItemHeader hdr;
    hdr.type = htole16(type);
    hdr.size = htole16(static_cast<uint16_t>(data.size()));
    std::vector<uint8_t> result(sizeof(debug_token::ItemHeader) + data.size());
    std::memcpy(result.data(), &hdr, sizeof(debug_token::ItemHeader));
    std::memcpy(result.data() + sizeof(debug_token::ItemHeader), data.data(),
                data.size());
    return result;
}

// Helper to build a raw TLV structure
static std::vector<uint8_t>
    buildRawStructure(uint16_t vMajor, uint16_t vMinor,
                      const std::vector<std::vector<uint8_t>>& rawItems)
{
    size_t payloadSize = 0;
    for (const auto& item : rawItems)
        payloadSize += item.size();
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

TEST_F(TestTokenUtilityExpanded, ParseSingleTlvRecordValidRecord)
{
    // Build a valid TLV structure with DeviceSerialNumber item
    auto serialItem =
        buildRawItem(0x0003, // DeviceSerialNumber
                     {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    auto encoded = buildRawStructure(2, 0, {serialItem});

    TokenMap tokens;
    size_t recordSize = 0;
    int result =
        TokenUtility::parseSingleTlvRecord(encoded, 0, tokens, recordSize);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_GT(recordSize, 0u);
}

TEST_F(TestTokenUtilityExpanded, ParseTlvTokensValidSingleRecord)
{
    // Build a TLV token with serial number
    auto serialItem =
        buildRawItem(0x0003, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22});
    auto tokenData = buildRawStructure(2, 0, {serialItem});

    // Build file: header + token
    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 1;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + tokenData.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(DebugTokenHeader));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), tokenData.data(),
                tokenData.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
}

// ========================== More parseQueryV2Response branches
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2ExceptionInParsing)
{
    // Provide non-hex string at completion code position to trigger exception
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "ZZ"; // invalid hex
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2NonZeroCompletionCode)
{
    // 37-byte response with non-zero completion code
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "01"; // completion code = 1 (error)
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1); // Returns -1 on non-success completion
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2TokenInstalledWithType)
{
    // 37-byte response, completion=0, token installed, type = 0x04 (hw unlock)
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00"; // completion = success
    rxBytes[9] = "01"; // token installed
    // Token type bytes at positions 19-22
    rxBytes[19] = "04"; // type byte 0
    rxBytes[20] = "00"; // type byte 1
    rxBytes[21] = "00"; // type byte 2
    rxBytes[22] = "00"; // type byte 3
    int tokenInstallStatus = 0, installedTokenType = 0;
    updateDebugToken->parseQueryV2Response(rxBytes, tokenInstallStatus,
                                           installedTokenType, 31);
    EXPECT_EQ(tokenInstallStatus, 1);
    EXPECT_EQ(installedTokenType, 4);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2ExceptionInTokenStatus)
{
    // 37-byte response, completion=0, but invalid hex at token status byte
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00"; // completion = success
    rxBytes[9] = "ZZ"; // invalid hex - will throw
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded,
       ParseQueryV2ExceptionInInstalledTokenTypeBytes)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "01";
    rxBytes[19] = "GG";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded,
       ParseQueryV2WrongSizeWithInvalidCompletionCode)
{
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "GG"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV2Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV2AllocationFailureSweep)
{
    std::vector<std::string> valid(37, "00");
    valid[9] = "01";
    valid[19] = "04";

    std::vector<std::string> overflow(37, "00");
    overflow[9] = "FFFFFFFFFFFFFFFF";

    std::vector<std::string> badTokenStatus(37, "00");
    badTokenStatus[9] = "ZZ";

    std::vector<std::string> badTokenType(37, "00");
    badTokenType[9] = "01";
    badTokenType[19] = "ZZ";

    std::vector<std::string> shortResponse(3, "00");

    auto prepare = [&] { alloc_fail::reset(); };
    auto invoke = [&] {
        int tokenInstallStatus = 0;
        int installedTokenType = 0;
        (void)updateDebugToken->parseQueryV2Response(valid, tokenInstallStatus,
                                                     installedTokenType, 31);
        (void)updateDebugToken->parseQueryV2Response(
            overflow, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV2Response(
            badTokenStatus, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV2Response(
            badTokenType, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV2Response(
            shortResponse, tokenInstallStatus, installedTokenType, 31);
    };

    invoke();
    runMeasuredAllocFailureSweep(prepare, invoke);
}

// ========================== parseQueryV3Response Tests
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3ResponseTooShort)
{
    std::vector<std::string> rxBytes = {"00", "00", "16"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3NonZeroCompletionCode)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "01";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3SuccessTokenNotInstalled)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "00";
    rxBytes[tokenInstallStatusByteV3] = "00";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 0);
    EXPECT_EQ(installedTokenType, 0);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3SuccessTokenInstalled)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "00";
    rxBytes[tokenInstallStatusByteV3] = "01";
    rxBytes[tokenTypeByteStartV3] = "08";
    rxBytes[tokenTypeByteStartV3 + 1] = "00";
    rxBytes[tokenTypeByteStartV3 + 2] = "00";
    rxBytes[tokenTypeByteStartV3 + 3] = "00";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 1);
    EXPECT_EQ(installedTokenType, 8);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3ExceptionInParsing)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "ZZ";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3ExceptionInTokenStatus)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "00";
    rxBytes[tokenInstallStatusByteV3] = "ZZ";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded,
       ParseQueryV3ExceptionInInstalledTokenTypeBytes)
{
    std::vector<std::string> rxBytes(mctpDebugTokenQueryResponseLengthV3, "00");
    rxBytes[mctpCompletionCodeByte] = "00";
    rxBytes[tokenInstallStatusByteV3] = "01";
    rxBytes[tokenTypeByteStartV3] = "GG";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded,
       ParseQueryV3WrongSizeWithInvalidCompletionCode)
{
    std::vector<std::string> rxBytes(mctpCompletionCodeByte + 1, "00");
    rxBytes[mctpCompletionCodeByte] = "GG";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = updateDebugToken->parseQueryV3Response(
        rxBytes, tokenInstallStatus, installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(TestUpdateDebugTokenExpanded, ParseQueryV3AllocationFailureSweep)
{
    std::vector<std::string> valid(50, "00");
    valid[12] = "01";
    valid[30] = "04";

    std::vector<std::string> overflow(50, "00");
    overflow[30] = "FFFFFFFFFFFFFFFF";

    std::vector<std::string> badTokenStatus(50, "00");
    badTokenStatus[12] = "ZZ";

    std::vector<std::string> badTokenType(50, "00");
    badTokenType[12] = "01";
    badTokenType[30] = "ZZ";

    std::vector<std::string> shortResponse(3, "00");

    auto prepare = [&] { alloc_fail::reset(); };
    auto invoke = [&] {
        int tokenInstallStatus = 0;
        int installedTokenType = 0;
        (void)updateDebugToken->parseQueryV3Response(valid, tokenInstallStatus,
                                                     installedTokenType, 31);
        (void)updateDebugToken->parseQueryV3Response(
            overflow, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV3Response(
            badTokenStatus, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV3Response(
            badTokenType, tokenInstallStatus, installedTokenType, 31);
        (void)updateDebugToken->parseQueryV3Response(
            shortResponse, tokenInstallStatus, installedTokenType, 31);
    };

    invoke();
    runMeasuredAllocFailureSweep(prepare, invoke);
}

// ========================== More updateTokenMap branches
// ==========================

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapTlvV2Format)
{
    // Create a TLV v2 token file
    std::string tmpPath = "/tmp/test_tlv_v2_token.bin";
    {
        // Build TLV structure with serial number
        auto serialItem = buildRawItem(
            0x0003, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22});
        auto tokenData = buildRawStructure(2, 0, {serialItem});

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2; // TLV v2 format
        fileHdr.type = 2;    // FileTypeDebugToken
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        fileHdr.fileSize =
            sizeof(DebugTokenHeader) + static_cast<uint32_t>(tokenData.size());

        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(tokenData.data()),
                tokenData.size());
    }
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapTlvMissingSerialFails)
{
    std::string tmpPath = "/tmp/test_tlv_missing_serial.bin";
    {
        auto typeItem = buildRawItem(0x0001, {0x01});
        auto tokenData = buildRawStructure(2, 0, {typeItem});

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        fileHdr.fileSize =
            sizeof(DebugTokenHeader) + static_cast<uint32_t>(tokenData.size());

        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(tokenData.data()),
                tokenData.size());
    }

    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapTlvMixedValidAndMalformed)
{
    std::string tmpPath = "/tmp/test_tlv_mixed_records.bin";
    {
        auto serialItem = buildRawItem(
            0x0003, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22});
        auto validToken = buildRawStructure(2, 0, {serialItem});

        debug_token::StructureHeader badHeader{};
        std::memcpy(badHeader.identifier, "BAD!", 4);
        badHeader.versionMajor = htole16(2);
        badHeader.versionMinor = htole16(0);
        badHeader.size = htole32(0);

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 2;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        fileHdr.fileSize = sizeof(DebugTokenHeader) +
                           static_cast<uint32_t>(validToken.size()) +
                           sizeof(debug_token::StructureHeader);

        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(validToken.data()),
                validToken.size());
        f.write(reinterpret_cast<const char*>(&badHeader), sizeof(badHeader));
    }

    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);

    EXPECT_EQ(result, -1);
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0xAABBCCDDEEFF1122"));
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapLegacyInvalidTokenSize)
{
    // Legacy format where token header says structSize != actual data
    std::string tmpPath = "/tmp/test_bad_size_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.structSize =
            999; // Mismatch - claims 999 but we have 256 bytes
        std::vector<uint8_t> data(256, 0);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));

        DebugTokenHeader fileHdr{};
        fileHdr.version = 1;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);
    // Should fail because token data is truncated (structSize > available)
    EXPECT_EQ(result, -1);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenExpanded, UpdateTokenMapMultipleLegacyTokens)
{
    std::string tmpPath = "/tmp/test_multi_token.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        // Two tokens of 128 bytes each
        size_t tokenSize = 128;
        std::vector<uint8_t> serial1 = {0x01, 0x02, 0x03, 0x04,
                                        0x05, 0x06, 0x07, 0x08};
        std::vector<uint8_t> serial2 = {0x11, 0x12, 0x13, 0x14,
                                        0x15, 0x16, 0x17, 0x18};

        auto makeToken = [&](const std::vector<uint8_t>& serial) {
            TokenHeader hdr{};
            std::memcpy(hdr.identifier, "EDTI", 4);
            hdr.structSize = static_cast<uint16_t>(tokenSize);
            std::vector<uint8_t> data(tokenSize, 0);
            std::memcpy(data.data(), &hdr, sizeof(TokenHeader));
            std::memcpy(data.data() + sizeof(TokenHeader), serial.data(),
                        serial.size());
            return data;
        };

        auto tok1 = makeToken(serial1);
        auto tok2 = makeToken(serial2);

        DebugTokenHeader fileHdr{};
        fileHdr.version = 1;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 2;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(tok1.data()), tok1.size());
        f.write(reinterpret_cast<const char*>(tok2.data()), tok2.size());
    }
    TokenMap tokens;
    int result = updateDebugToken->updateTokenMap(tmpPath, tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 2u);
    std::filesystem::remove(tmpPath);
}

// ========================== More token_utility branches
// ==========================

TEST_F(TestTokenUtilityExpanded, ParseSingleTlvRecordMissingSerial)
{
    // TLV structure with DeviceType but no DeviceSerialNumber
    auto typeItem = buildRawItem(0x0001, {0x01}); // DeviceType
    auto encoded = buildRawStructure(2, 0, {typeItem});

    TokenMap tokens;
    size_t recordSize = 0;
    int result =
        TokenUtility::parseSingleTlvRecord(encoded, 0, tokens, recordSize);
    // Should succeed but token has no serial -> skipped
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestTokenUtilityExpanded, ParseTlvTokensMultipleRecords)
{
    auto serial1 =
        buildRawItem(0x0003, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    auto token1 = buildRawStructure(2, 0, {serial1});

    auto serial2 =
        buildRawItem(0x0003, {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18});
    auto token2 = buildRawStructure(2, 0, {serial2});

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 2;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + token1.size() +
                                  token2.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(DebugTokenHeader));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), token1.data(),
                token1.size());
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader) + token1.size(),
                token2.data(), token2.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 2u);
}

TEST_F(TestTokenUtilityExpanded, ParseTlvTokensRecordCountExceedsData)
{
    // Header says 5 records but only 1 is in the file
    auto serial =
        buildRawItem(0x0003, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    auto tokenData = buildRawStructure(2, 0, {serial});

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 5; // Claims 5 records
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + tokenData.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(DebugTokenHeader));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), tokenData.data(),
                tokenData.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);
    // Should parse the 1 valid record but return error for missing remaining
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_EQ(result, -1); // Error because not all records found
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenTruncatedBody)
{
    // Header is valid but body is truncated (structSize > file size)
    std::string tmpPath = "/tmp/test_trunc_body.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.structSize = 512; // Claims 512 but only 64 bytes written
        std::vector<uint8_t> data(64, 0);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto hdr = getNextDebugToken(pkg, 0, tokenData, serialNumber);
    EXPECT_EQ(hdr, nullptr); // Should fail due to truncation
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, ExtractSerialNumberFromTlvValid)
{
    auto serialItem =
        buildRawItem(0x0003, {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80});
    auto encoded = buildRawStructure(2, 0, {serialItem});
    debug_token::tlv_decoder::Structure tlv(encoded);

    EXPECT_EQ(TokenUtility::extractSerialNumberFromTlv(tlv),
              "0x1020304050607080");
}

TEST_F(TestTokenUtilityExpanded, ExtractSerialNumberFromTlvMissingReturnsEmpty)
{
    auto typeItem = buildRawItem(0x0001, {0x01});
    auto encoded = buildRawStructure(2, 0, {typeItem});
    debug_token::tlv_decoder::Structure tlv(encoded);

    EXPECT_TRUE(TokenUtility::extractSerialNumberFromTlv(tlv).empty());
}

TEST_F(TestTokenUtilityExpanded, ParseSingleTlvRecordHeaderOutOfBounds)
{
    std::vector<uint8_t> encoded(sizeof(debug_token::StructureHeader) - 1, 0);
    TokenMap tokens;
    size_t recordSize = 0;

    int result =
        TokenUtility::parseSingleTlvRecord(encoded, 0, tokens, recordSize);

    EXPECT_EQ(result, -1);
    EXPECT_EQ(recordSize, 0u);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestTokenUtilityExpanded, ParseSingleTlvRecordStructureOutOfBounds)
{
    debug_token::StructureHeader hdr{};
    std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(32);

    std::vector<uint8_t> encoded(sizeof(debug_token::StructureHeader) + 1, 0);
    std::memcpy(encoded.data(), &hdr, sizeof(hdr));

    TokenMap tokens;
    size_t recordSize = 0;
    int result =
        TokenUtility::parseSingleTlvRecord(encoded, 0, tokens, recordSize);

    EXPECT_EQ(result, -1);
    EXPECT_EQ(recordSize, sizeof(debug_token::StructureHeader) + 32u);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestTokenUtilityExpanded,
       ParseTlvTokensMalformedFirstRecordKeepsLaterValid)
{
    debug_token::StructureHeader badHeader{};
    std::memcpy(badHeader.identifier, "BAD!", 4);
    badHeader.versionMajor = htole16(2);
    badHeader.versionMinor = htole16(0);
    badHeader.size = htole32(0);

    std::vector<uint8_t> badRecord(sizeof(debug_token::StructureHeader));
    std::memcpy(badRecord.data(), &badHeader, sizeof(badHeader));

    auto serialItem =
        buildRawItem(0x0003, {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28});
    auto validRecord = buildRawStructure(2, 0, {serialItem});

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 2;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + badRecord.size() +
                                  validRecord.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(fileHdr));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), badRecord.data(),
                badRecord.size());
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader) + badRecord.size(),
                validRecord.data(), validRecord.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);

    EXPECT_EQ(result, -1);
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x2122232425262728"));
}

TEST_F(TestTokenUtilityExpanded, ParseTlvTokensAllRecordsMissingSerial)
{
    auto record1 = buildRawStructure(2, 0, {buildRawItem(0x0001, {0x01})});
    auto record2 = buildRawStructure(2, 0, {buildRawItem(0x0001, {0x02})});

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 2;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + record1.size() +
                                  record2.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(fileHdr));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), record1.data(),
                record1.size());
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader) + record1.size(),
                record2.data(), record2.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestTokenUtilityExpanded, ParseTlvTokensStopsOnDeclaredStructurePastEnd)
{
    debug_token::StructureHeader badHeader{};
    std::memcpy(badHeader.identifier, debug_token::TLV_IDENTIFIER, 4);
    badHeader.versionMajor = htole16(2);
    badHeader.versionMinor = htole16(0);
    badHeader.size = htole32(128);

    std::vector<uint8_t> badRecord(sizeof(debug_token::StructureHeader) + 4, 0);
    std::memcpy(badRecord.data(), &badHeader, sizeof(badHeader));

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 1;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + badRecord.size());
    std::memcpy(fullFile.data(), &fileHdr, sizeof(fileHdr));
    std::memcpy(fullFile.data() + sizeof(DebugTokenHeader), badRecord.data(),
                badRecord.size());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &fileHdr, tokens);

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenReadsTokenAtNonZeroOffset)
{
    std::string tmpPath = "/tmp/test_next_token_offset.bin";
    constexpr size_t tokenSize = 96;
    {
        std::ofstream f(tmpPath, std::ios::binary);

        auto writeToken = [&](const char* ident, uint8_t serialSeed) {
            TokenHeader tokenHdr{};
            std::memcpy(tokenHdr.identifier, ident, 4);
            tokenHdr.structSize = tokenSize;
            std::vector<uint8_t> data(tokenSize, 0);
            std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
            for (size_t i = 0; i < tokenSerialNumberSizeDefault; ++i)
            {
                data[sizeof(TokenHeader) + i] =
                    static_cast<uint8_t>(serialSeed + i);
            }
            f.write(reinterpret_cast<const char*>(data.data()), data.size());
        };

        writeToken("EDTI", 0x10);
        writeToken("EDTI", 0x20);
    }

    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto* hdr = getNextDebugToken(pkg, tokenSize, tokenData, serialNumber);

    ASSERT_NE(hdr, nullptr);
    ASSERT_EQ(serialNumber.size(), tokenSerialNumberSizeDefault);
    EXPECT_EQ(serialNumber.front(), 0x20);
    EXPECT_EQ(serialNumber.back(), 0x27);
    EXPECT_EQ(tokenData.size(), tokenSize);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestTokenUtilityExpanded, GetNextDebugTokenOffsetPastEndReturnsNull)
{
    std::string tmpPath = "/tmp/test_next_token_offset_past_end.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.structSize = 64;
        std::vector<uint8_t> data(64, 0);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    std::ifstream pkg(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData{0xAA};
    std::vector<uint8_t> serialNumber;
    auto* hdr = getNextDebugToken(pkg, 4096, tokenData, serialNumber);

    EXPECT_EQ(hdr, nullptr);
    EXPECT_TRUE(tokenData.empty());
    std::filesystem::remove(tmpPath);
}

// ========================== Tests that need mock bus (D-Bus calls)
// ============

#include <sdbusplus/test/sdbus_mock.hpp>

class TestUpdateDebugTokenMockBus : public testing::Test
{
  public:
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    std::unique_ptr<UpdateDebugToken> udt;
    TestUpdateDebugTokenMockBus()
    {
        udt = std::make_unique<UpdateDebugToken>(bus);
    }
};

// ========================== createMessageRegistry ==========================

TEST_F(TestUpdateDebugTokenMockBus, CreateMessageRegistryUpdateSuccessful)
{
    EXPECT_NO_THROW(udt->createMessageRegistry("Update.1.0.UpdateSuccessful",
                                               "DebugToken", "1.0"));
}

TEST_F(TestUpdateDebugTokenMockBus,
       CreateMessageRegistryDbusFailureDoesNotThrow)
{
    EXPECT_CALL(sdbusMock,
                sd_bus_call(nullptr, nullptr, testing::_, testing::_, nullptr))
        .WillOnce(testing::Return(-1));

    EXPECT_NO_THROW(udt->createMessageRegistry("Update.1.0.UpdateSuccessful",
                                               "DebugToken", "1.0"));
}

TEST_F(TestUpdateDebugTokenMockBus, CreateMessageRegistryTransferFailed)
{
    EXPECT_NO_THROW(udt->createMessageRegistry("Update.1.0.TransferFailed",
                                               "DebugToken", "1.0"));
}

// ========================== createTokenInstallErrorMessage /
// createTokenEraseErrorMessage ====

TEST_F(TestUpdateDebugTokenMockBus, CreateTokenInstallErrorMessage)
{
    EXPECT_NO_THROW(udt->createTokenInstallErrorMessage("GPU_ERoT_0"));
}

TEST_F(TestUpdateDebugTokenMockBus, CreateTokenEraseErrorMessage)
{
    EXPECT_NO_THROW(udt->createTokenEraseErrorMessage("GPU_ERoT_0"));
}

// ========================== createMessageRegistryResourceErrors
// ==========================

TEST_F(TestUpdateDebugTokenMockBus, CreateMsgRegistryResourceErrorsKnown)
{
    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "ResourceEvent.1.0.ResourceErrorsDetected", "DebugTokenInstall",
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::TokenParseFailure)));
}

TEST_F(TestUpdateDebugTokenMockBus, CreateMsgRegistryResourceErrorsWithDevice)
{
    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "ResourceEvent.1.0.ResourceErrorsDetected", "DebugTokenInstall",
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::InvalidToken), "GPU_ERoT_0"));
}

TEST_F(TestUpdateDebugTokenMockBus, CreateMsgRegistryResourceErrorsUnknownCode)
{
    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "Component", OperationType::TokenInstall,
        999, "device"));
}

TEST_F(TestUpdateDebugTokenMockBus, CreateMsgRegistryResourceErrorsSuccessMsg)
{
    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "Update.1.0.UpdateSuccessful", "DebugTokenInstall",
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPCommandInstallSuccess),
        "GPU_0"));
}

TEST_F(TestUpdateDebugTokenMockBus,
       CreateMsgRegistryResourceErrorsAllOperations)
{
    // Cover all OperationType branches with known codes
    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "ResourceEvent.1.0.ResourceErrorsDetected", "Comp",
        OperationType::TokenErase,
        static_cast<int>(EraseErrorCodes::EraseInternalError), "dev"));

    EXPECT_NO_THROW(udt->createMessageRegistryResourceErrors(
        "ResourceEvent.1.0.ResourceErrorsDetected", "Comp",
        OperationType::BackgroundCopy,
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundEnableFail),
        "dev"));
}

// ========================== installDebugToken ==========================

TEST_F(TestUpdateDebugTokenMockBus, InstallDebugTokenFileNotFound)
{
    auto status = udt->installDebugToken("/nonexistent/token.bin");
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
}

TEST_F(TestUpdateDebugTokenMockBus, InstallDebugTokenInvalidFile)
{
    std::string tmpPath = "/tmp/test_install_invalid.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = 99;
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    auto status = udt->installDebugToken(tmpPath);
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
    std::filesystem::remove(tmpPath);
}

// ========================== eraseDebugToken ==========================

TEST_F(TestUpdateDebugTokenMockBus, EraseDebugTokenDBusError)
{
    // getErasePolicy() D-Bus call fails -> returns empty -> not "Manual"
    // nsmTokenEraseV2() D-Bus call fails -> returns -1
    int result = udt->eraseDebugToken();
    EXPECT_NE(result, 0);
}

TEST_F(TestUpdateDebugTokenMockBus, InstallDebugTokenValidFileNsmFails)
{
    // Create a valid TLV v2 token file so updateTokenMap succeeds,
    // then nsmTokenInstallV2 will be called and fail (D-Bus mock error)
    std::string tmpPath = "/tmp/test_install_valid_nsm_fail.bin";
    {
        auto serialItem = buildRawItem(
            0x0003, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22});
        auto tokenData = buildRawStructure(2, 0, {serialItem});

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(tokenData.data()),
                tokenData.size());
    }
    auto status = udt->installDebugToken(tmpPath);
    // updateTokenMap succeeds, nsmTokenInstallV2 fails (D-Bus error)
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
    std::filesystem::remove(tmpPath);
}

TEST_F(TestUpdateDebugTokenMockBus, InstallDebugTokenValidLegacyNsmFails)
{
    // Legacy format token -> updateTokenMap succeeds -> nsmTokenInstallV2 fails
    std::string tmpPath = "/tmp/test_install_legacy_nsm_fail.bin";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        TokenHeader tokenHdr{};
        std::memcpy(tokenHdr.identifier, "EDTI", 4);
        tokenHdr.structSize = 128;
        std::vector<uint8_t> data(128, 0);
        std::memcpy(data.data(), &tokenHdr, sizeof(TokenHeader));
        // serial at offset sizeof(TokenHeader)
        uint8_t serial[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        std::memcpy(data.data() + sizeof(TokenHeader), serial, 8);

        DebugTokenHeader fileHdr{};
        fileHdr.version = 1;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    auto status = udt->installDebugToken(tmpPath);
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
    std::filesystem::remove(tmpPath);
}

// createLog is private in UpdateDebugToken - tested indirectly via
// createMessageRegistry and createMessageRegistryResourceErrors above

// ========================== More MctpEidInfo tests ==========================

TEST_F(TestUpdateDebugTokenExpanded, MctpEidInfoComparisonUSBvsSMBus)
{
    MctpEidInfo a{1,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB",
                  {},
                  true};
    MctpEidInfo b{2,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus",
                  {},
                  true};
    // USB(1) vs SMBus(5) medium priority
    EXPECT_TRUE(b < a);
}

TEST_F(TestUpdateDebugTokenExpanded, MctpEidInfoEqualPriority)
{
    MctpEidInfo a{1,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                  {},
                  true};
    MctpEidInfo b{2,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                  {},
                  true};
    // Same medium, same binding - neither is less
    EXPECT_FALSE(a < b);
    EXPECT_FALSE(b < a);
}

TEST_F(TestUpdateDebugTokenExpanded,
       FetchEidInfoFromObjectWithEidOnlyLeavesDefaults)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = {
        {"EID", static_cast<uint8_t>(42)},
        {"MediumType",
         std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}};
    interfaces[mctpEndpointEnableIntfName] = {
        {"Connectivity", std::string("Available")}};

    auto info = updateDebugToken->fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_TRUE(info.enabled);
}

TEST_F(TestUpdateDebugTokenExpanded,
       FetchEidInfoFromObjectWithSupportedTypesOnlyLeavesEidUnset)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = {
        {"SupportedMessageTypes",
         std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
        {"MediumType",
         std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}};
    interfaces[mctpEndpointEnableIntfName] = {
        {"Connectivity", std::string("Available")}};

    auto info = updateDebugToken->fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_TRUE(info.enabled);
}

TEST_F(TestUpdateDebugTokenExpanded,
       FetchEidInfoFromObjectMissingConnectivityPropertyLeavesDisabled)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = {
        {"EID", static_cast<uint8_t>(52)},
        {"SupportedMessageTypes",
         std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
        {"MediumType",
         std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}};
    interfaces[mctpBindingIntfName] = {
        {"BindingType",
         std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}};
    interfaces[mctpEndpointEnableIntfName] = {};

    auto info = updateDebugToken->fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 52);
    EXPECT_FALSE(info.enabled);
    EXPECT_EQ(info.binding,
              "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
}

TEST_F(TestUpdateDebugTokenExpanded,
       FetchEidInfoFromObjectWithoutEnableInterfaceLeavesDisabled)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = {
        {"EID", static_cast<uint8_t>(61)},
        {"SupportedMessageTypes",
         std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
        {"MediumType",
         std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}};
    interfaces[mctpBindingIntfName] = {
        {"BindingType",
         std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")}};

    auto info = updateDebugToken->fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 61);
    EXPECT_FALSE(info.enabled);
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
}
