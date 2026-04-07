/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../debug_token/tlv/tlv.h"
#include "../debug_token/tlv/types.h"

#include <endian.h>

#include <array>
#include <cstring>
#include <set>
#include <vector>

#include "gtest/gtest.h"

using namespace debug_token;
using namespace debug_token::tlv_decoder;

// Helper to build a raw TLV Item (type + size + data)
static std::vector<uint8_t> makeRawItem(uint16_t type,
                                        const std::vector<uint8_t>& data)
{
    ItemHeader hdr;
    hdr.type = htole16(type);
    hdr.size = htole16(static_cast<uint16_t>(data.size()));
    std::vector<uint8_t> result(sizeof(ItemHeader) + data.size());
    std::memcpy(result.data(), &hdr, sizeof(ItemHeader));
    if (!data.empty())
    {
        std::memcpy(result.data() + sizeof(ItemHeader), data.data(),
                    data.size());
    }
    return result;
}

// Helper to build a complete TLV Structure binary
static std::vector<uint8_t>
    makeRawStructure(uint16_t vMajor, uint16_t vMinor,
                     const std::vector<std::vector<uint8_t>>& rawItems)
{
    size_t payloadSize = 0;
    for (const auto& item : rawItems)
    {
        payloadSize += item.size();
    }

    StructureHeader hdr{};
    std::memcpy(hdr.identifier, TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(vMajor);
    hdr.versionMinor = htole16(vMinor);
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));

    std::vector<uint8_t> result(sizeof(StructureHeader) + payloadSize);
    std::memcpy(result.data(), &hdr, sizeof(StructureHeader));
    size_t offset = sizeof(StructureHeader);
    for (const auto& item : rawItems)
    {
        std::memcpy(result.data() + offset, item.data(), item.size());
        offset += item.size();
    }
    return result;
}

template <typename T>
static std::vector<uint8_t> makeScalarValueBytes(T value)
{
    T encoded = value;
    if constexpr (sizeof(T) == sizeof(uint16_t))
    {
        encoded = static_cast<T>(htole16(static_cast<uint16_t>(value)));
    }
    else if constexpr (sizeof(T) == sizeof(uint32_t))
    {
        encoded = static_cast<T>(htole32(static_cast<uint32_t>(value)));
    }
    else if constexpr (sizeof(T) == sizeof(uint64_t))
    {
        encoded = static_cast<T>(htole64(static_cast<uint64_t>(value)));
    }

    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &encoded, sizeof(T));
    return bytes;
}

template <typename T>
static std::vector<uint8_t> makeVectorValueBytes(const std::vector<T>& values)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(values.size() * sizeof(T));
    for (const auto& value : values)
    {
        auto encoded = makeScalarValueBytes(value);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return bytes;
}

// ========================== Item Tests ==========================

TEST(TlvDecoderItem, ConstructValidItem)
{
    auto raw = makeRawItem(0x0001, {0xAB, 0xCD});
    EXPECT_NO_THROW(Item item(std::span{raw}));
}

TEST(TlvDecoderItem, ConstructTooShort)
{
    std::vector<uint8_t> raw = {0x01}; // shorter than ItemHeader
    EXPECT_THROW(Item item(std::span{raw}), std::runtime_error);
}

TEST(TlvDecoderItem, ConstructDataTooShort)
{
    // Header claims 10 bytes of data but only 2 are provided
    ItemHeader hdr;
    hdr.type = htole16(0x0001);
    hdr.size = htole16(10);
    std::vector<uint8_t> raw(sizeof(ItemHeader) + 2);
    std::memcpy(raw.data(), &hdr, sizeof(ItemHeader));
    EXPECT_THROW(Item item(std::span{raw}), std::runtime_error);
}

TEST(TlvDecoderItem, GetType)
{
    auto raw = makeRawItem(0x0003, {0x01, 0x02, 0x03});
    Item item(std::span{raw});
    EXPECT_EQ(item.getType(), 0x0003);
}

TEST(TlvDecoderItem, GetTotalSize)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getTotalSize(), sizeof(ItemHeader) + 5);
}

TEST(TlvDecoderItem, GetValueSize)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getValueSize(), 3u);
}

TEST(TlvDecoderItem, GetRawValue)
{
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getRawValue(), data);
}

TEST(TlvDecoderItem, GetValueUint8)
{
    auto raw = makeRawItem(0x0001, {0x42});
    Item item(std::span{raw});
    EXPECT_EQ(item.getValue<uint8_t>(), 0x42);
}

TEST(TlvDecoderItem, GetValueUint16)
{
    uint16_t val = 0x1234;
    uint16_t le_val = htole16(val);
    std::vector<uint8_t> data(2);
    std::memcpy(data.data(), &le_val, 2);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getValue<uint16_t>(), val);
}

TEST(TlvDecoderItem, GetValueUint32)
{
    uint32_t val = 0xDEADBEEF;
    uint32_t le_val = htole32(val);
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &le_val, 4);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getValue<uint32_t>(), val);
}

TEST(TlvDecoderItem, GetValueUint64)
{
    uint64_t val = 0x0102030405060708ULL;
    uint64_t le_val = htole64(val);
    std::vector<uint8_t> data(8);
    std::memcpy(data.data(), &le_val, 8);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    EXPECT_EQ(item.getValue<uint64_t>(), val);
}

TEST(TlvDecoderItem, GetValueUint16WrongSize)
{
    // 3 bytes can't be a uint16_t
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<uint16_t>(), std::runtime_error);
}

TEST(TlvDecoderItem, GetValueUint32WrongSize)
{
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<uint32_t>(), std::runtime_error);
}

TEST(TlvDecoderItem, GetValueUint64WrongSize)
{
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<uint64_t>(), std::runtime_error);
}

TEST(TlvDecoderItem, GetValueVectorUint8)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint8_t>>();
    EXPECT_EQ(result, data);
}

TEST(TlvDecoderItem, GetValueVectorUint16)
{
    uint16_t v1 = htole16(0x1234);
    uint16_t v2 = htole16(0x5678);
    std::vector<uint8_t> data(4);
    std::memcpy(data.data(), &v1, 2);
    std::memcpy(data.data() + 2, &v2, 2);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint16_t>>();
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 0x1234);
    EXPECT_EQ(result[1], 0x5678);
}

TEST(TlvDecoderItem, GetValueVectorUint16Empty)
{
    auto raw = makeRawItem(0x0001, {});
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint16_t>>();
    EXPECT_TRUE(result.empty());
}

TEST(TlvDecoderItem, GetValueVectorUint16ThreeValues)
{
    uint16_t v1 = htole16(0x1234);
    uint16_t v2 = htole16(0x5678);
    uint16_t v3 = htole16(0x9ABC);
    std::vector<uint8_t> data(6);
    std::memcpy(data.data(), &v1, 2);
    std::memcpy(data.data() + 2, &v2, 2);
    std::memcpy(data.data() + 4, &v3, 2);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint16_t>>();
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 0x1234);
    EXPECT_EQ(result[1], 0x5678);
    EXPECT_EQ(result[2], 0x9ABC);
}

TEST(TlvDecoderItem, GetValueVectorUint32)
{
    uint32_t v1 = htole32(0xDEADBEEF);
    uint32_t v2 = htole32(0xCAFEBABE);
    std::vector<uint8_t> data(8);
    std::memcpy(data.data(), &v1, 4);
    std::memcpy(data.data() + 4, &v2, 4);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint32_t>>();
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 0xDEADBEEF);
    EXPECT_EQ(result[1], 0xCAFEBABE);
}

TEST(TlvDecoderItem, GetValueVectorUint32Empty)
{
    auto raw = makeRawItem(0x0001, {});
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint32_t>>();
    EXPECT_TRUE(result.empty());
}

TEST(TlvDecoderItem, GetValueVectorUint32ThreeValues)
{
    uint32_t v1 = htole32(0x01020304);
    uint32_t v2 = htole32(0xAABBCCDD);
    uint32_t v3 = htole32(0x10203040);
    std::vector<uint8_t> data(12);
    std::memcpy(data.data(), &v1, 4);
    std::memcpy(data.data() + 4, &v2, 4);
    std::memcpy(data.data() + 8, &v3, 4);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint32_t>>();
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 0x01020304u);
    EXPECT_EQ(result[1], 0xAABBCCDDu);
    EXPECT_EQ(result[2], 0x10203040u);
}

TEST(TlvDecoderItem, GetValueVectorUint64)
{
    uint64_t v1 = htole64(0x0102030405060708ULL);
    std::vector<uint8_t> data(8);
    std::memcpy(data.data(), &v1, 8);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint64_t>>();
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 0x0102030405060708ULL);
}

TEST(TlvDecoderItem, GetValueVectorUint64Empty)
{
    auto raw = makeRawItem(0x0001, {});
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint64_t>>();
    EXPECT_TRUE(result.empty());
}

TEST(TlvDecoderItem, GetValueVectorUint64TwoValues)
{
    uint64_t v1 = htole64(0x0102030405060708ULL);
    uint64_t v2 = htole64(0x1112131415161718ULL);
    std::vector<uint8_t> data(16);
    std::memcpy(data.data(), &v1, 8);
    std::memcpy(data.data() + 8, &v2, 8);
    auto raw = makeRawItem(0x0001, data);
    Item item(std::span{raw});
    auto result = item.getValue<std::vector<uint64_t>>();
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 0x0102030405060708ULL);
    EXPECT_EQ(result[1], 0x1112131415161718ULL);
}

TEST(TlvDecoderItem, GetValueVectorUint16WrongSize)
{
    // 3 bytes is not multiple of 2
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<std::vector<uint16_t>>(), std::runtime_error);
}

TEST(TlvDecoderItem, GetValueVectorUint32WrongSize)
{
    // 3 bytes is not multiple of 4
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<std::vector<uint32_t>>(), std::runtime_error);
}

TEST(TlvDecoderItem, GetValueVectorUint64WrongSize)
{
    // 5 bytes is not multiple of 8
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04, 0x05});
    Item item(std::span{raw});
    EXPECT_THROW(item.getValue<std::vector<uint64_t>>(), std::runtime_error);
}

// ========================== Item::getTypeName Tests ==========================

TEST(TlvDecoderItem, GetTypeNameCommon)
{
    EXPECT_EQ(Item::getTypeName(types::Common::DeviceType), "DeviceType");
    EXPECT_EQ(Item::getTypeName(types::Common::ChallengeNonce),
              "ChallengeNonce");
    EXPECT_EQ(Item::getTypeName(types::Common::DeviceSerialNumber),
              "DeviceSerialNumber");
    EXPECT_EQ(Item::getTypeName(types::Common::DeviceSerialNumberArray),
              "DeviceSerialNumberArray");
    EXPECT_EQ(Item::getTypeName(types::Common::FirmwareVersion),
              "FirmwareVersion");
    EXPECT_EQ(Item::getTypeName(types::Common::AgentVersion), "AgentVersion");
    EXPECT_EQ(Item::getTypeName(types::Common::LifecycleState),
              "LifecycleState");
    EXPECT_EQ(Item::getTypeName(types::Common::TokenIdentifier),
              "TokenIdentifier");
    EXPECT_EQ(Item::getTypeName(types::Common::TokenType), "TokenType");
    EXPECT_EQ(Item::getTypeName(types::Common::TokenConfig), "TokenConfig");
    EXPECT_EQ(Item::getTypeName(types::Common::NvidiaSignature),
              "NvidiaSignature");
    EXPECT_EQ(Item::getTypeName(types::Common::OemSignature), "OemSignature");
    EXPECT_EQ(Item::getTypeName(types::Common::InstallationStatus),
              "InstallationStatus");
    EXPECT_EQ(Item::getTypeName(types::Common::ProcessingStatus),
              "ProcessingStatus");
    EXPECT_EQ(Item::getTypeName(types::Common::SkuInformation),
              "SkuInformation");
    EXPECT_EQ(Item::getTypeName(types::Common::NvidiaRatchet), "NvidiaRatchet");
    EXPECT_EQ(Item::getTypeName(types::Common::OemRatchet), "OemRatchet");
    EXPECT_EQ(Item::getTypeName(types::Common::ValidityCounter),
              "ValidityCounter");
    EXPECT_EQ(Item::getTypeName(types::Common::CertificateChain),
              "CertificateChain");
    EXPECT_EQ(Item::getTypeName(types::Common::MeasurementTranscript),
              "MeasurementTranscript");
    EXPECT_EQ(Item::getTypeName(types::Common::DeviceId), "DeviceId");
    EXPECT_EQ(Item::getTypeName(types::Common::TokenTypeSubtypeList),
              "TokenTypeSubtypeList");
    EXPECT_EQ(Item::getTypeName(types::Common::Payload), "Payload");
    EXPECT_EQ(Item::getTypeName(types::Common::LegacyToken), "LegacyToken");
}

TEST(TlvDecoderItem, GetTypeNameGPU)
{
    EXPECT_EQ(Item::getTypeName(types::GPU::FeatureMask), "GPUFeatureMask");
    EXPECT_EQ(Item::getTypeName(types::GPU::ChipId), "GPUChipId");
}

TEST(TlvDecoderItem, GetTypeNameNBU)
{
    EXPECT_EQ(Item::getTypeName(types::NBU::KeypairUUID), "NBUKeypairUUID");
    EXPECT_EQ(Item::getTypeName(types::NBU::PSID), "NBUPSID");
    EXPECT_EQ(Item::getTypeName(types::NBU::FileDeviceUnique),
              "NBUFileDeviceUnique");
}

TEST(TlvDecoderItem, GetTypeNameBMCIRoT)
{
    EXPECT_EQ(Item::getTypeName(types::BMCIRoT::TokenVersion),
              "BMCIRoTTokenVersion");
    EXPECT_EQ(Item::getTypeName(types::BMCIRoT::NvidiaSignatureAlgorithm),
              "BMCIRoTNvidiaSignatureAlgorithm");
}

TEST(TlvDecoderItem, GetTypeNameUnknown)
{
    auto name = Item::getTypeName(0xFFFF);
    EXPECT_TRUE(name.find("UnknownType") != std::string::npos);
}

TEST(TlvDecoderItem, GetTypeNameUnknownAcrossSwitchGaps)
{
    const std::vector<uint16_t> gapValues = {0x0000, 0x0019, 0x00FF, 0x3FFF,
                                             0x4002, 0x43FF, 0x4403, 0x47FF,
                                             0x4800, 0x4803};

    for (auto value : gapValues)
    {
        auto name = Item::getTypeName(value);
        EXPECT_TRUE(name.find("UnknownType") != std::string::npos)
            << "value=0x" << std::hex << value;
    }
}

TEST(TlvDecoderItem, GetTypeNameDenseRangeTraversal)
{
    const std::set<uint16_t> knownTypes = {
        types::Common::DeviceType,
        types::Common::ChallengeNonce,
        types::Common::DeviceSerialNumber,
        types::Common::DeviceSerialNumberArray,
        types::Common::FirmwareVersion,
        types::Common::AgentVersion,
        types::Common::LifecycleState,
        types::Common::TokenIdentifier,
        types::Common::TokenType,
        types::Common::TokenConfig,
        types::Common::NvidiaSignature,
        types::Common::OemSignature,
        types::Common::InstallationStatus,
        types::Common::ProcessingStatus,
        types::Common::SkuInformation,
        types::Common::NvidiaRatchet,
        types::Common::OemRatchet,
        types::Common::ValidityCounter,
        types::Common::CertificateChain,
        types::Common::MeasurementTranscript,
        types::Common::DeviceId,
        types::Common::TokenTypeSubtypeList,
        types::Common::Payload,
        types::Common::LegacyToken,
        types::GPU::FeatureMask,
        types::GPU::ChipId,
        types::NBU::KeypairUUID,
        types::NBU::PSID,
        types::NBU::FileDeviceUnique,
        types::BMCIRoT::TokenVersion,
        types::BMCIRoT::NvidiaSignatureAlgorithm,
    };

    const std::vector<std::pair<uint16_t, uint16_t>> denseRanges = {
        {0x0000, 0x0019},
        {0x3FFE, 0x4003},
        {0x43FE, 0x4403},
        {0x47FE, 0x4804},
    };

    for (const auto& [start, end] : denseRanges)
    {
        for (uint16_t value = start; value <= end; ++value)
        {
            const auto name = Item::getTypeName(value);
            const bool isKnown = knownTypes.contains(value);
            if (isKnown)
            {
                EXPECT_TRUE(name.find("UnknownType") == std::string::npos)
                    << "known value=0x" << std::hex << value;
            }
            else
            {
                EXPECT_TRUE(name.find("UnknownType") != std::string::npos)
                    << "unknown value=0x" << std::hex << value;
            }
        }
    }
}

TEST(TlvDecoderItem, GetValueScalarDenseCoverage)
{
    {
        auto raw =
            makeRawItem(0x0001, makeScalarValueBytes<unsigned char>(0x5A));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<unsigned char>(), 0x5A);
    }

    {
        auto raw =
            makeRawItem(0x0001, makeScalarValueBytes<unsigned short>(0x1234));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<unsigned short>(), 0x1234);
    }

    {
        auto raw = makeRawItem(0x0001,
                               makeScalarValueBytes<unsigned int>(0x89ABCDEFu));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<unsigned int>(), 0x89ABCDEFu);
    }

    {
        auto raw = makeRawItem(
            0x0001, makeScalarValueBytes<unsigned long>(0x0102030405060708UL));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<unsigned long>(), 0x0102030405060708UL);
    }

    for (size_t badSize = 0; badSize <= 2; ++badSize)
    {
        if (badSize == sizeof(unsigned char))
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0xAA));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<unsigned char>(), std::runtime_error);
    }

    for (size_t badSize = 0; badSize <= sizeof(unsigned short) + 1; ++badSize)
    {
        if (badSize == sizeof(unsigned short))
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0xBB));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<unsigned short>(), std::runtime_error);
    }

    for (size_t badSize = 0; badSize <= sizeof(unsigned int) + 1; ++badSize)
    {
        if (badSize == sizeof(unsigned int))
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0xCC));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<unsigned int>(), std::runtime_error);
    }

    for (size_t badSize = 0; badSize <= sizeof(unsigned long) + 1; ++badSize)
    {
        if (badSize == sizeof(unsigned long))
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0xDD));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<unsigned long>(), std::runtime_error);
    }
}

TEST(TlvDecoderItem, GetValueVectorDenseCoverage)
{
    {
        std::vector<unsigned short> values = {0x1111, 0x2222, 0x3333, 0x4444};
        auto raw = makeRawItem(0x0001, makeVectorValueBytes(values));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<std::vector<unsigned short>>(), values);
    }

    {
        std::vector<unsigned int> values = {0x01020304u, 0x11223344u,
                                            0x55667788u, 0x99AABBCCu};
        auto raw = makeRawItem(0x0001, makeVectorValueBytes(values));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<std::vector<unsigned int>>(), values);
    }

    {
        std::vector<unsigned long> values = {
            0x0102030405060708UL,
            0x1112131415161718UL,
            0x2122232425262728UL,
        };
        auto raw = makeRawItem(0x0001, makeVectorValueBytes(values));
        Item item(std::span{raw});
        EXPECT_EQ(item.getValue<std::vector<unsigned long>>(), values);
    }

    for (size_t badSize = 1; badSize <= 5; ++badSize)
    {
        if (badSize % sizeof(unsigned short) == 0)
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0x11));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<std::vector<unsigned short>>(),
                     std::runtime_error);
    }

    for (size_t badSize = 1; badSize <= 9; ++badSize)
    {
        if (badSize % sizeof(unsigned int) == 0)
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0x22));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<std::vector<unsigned int>>(),
                     std::runtime_error);
    }

    for (size_t badSize = 1; badSize <= 17; ++badSize)
    {
        if (badSize % sizeof(unsigned long) == 0)
        {
            continue;
        }
        auto raw = makeRawItem(0x0001, std::vector<uint8_t>(badSize, 0x33));
        Item item(std::span{raw});
        EXPECT_THROW(item.getValue<std::vector<unsigned long>>(),
                     std::runtime_error);
    }
}

// ========================== Structure Tests ==========================

TEST(TlvDecoderStructure, ConstructValid)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto item2 = makeRawItem(types::Common::DeviceSerialNumber,
                             {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    auto raw = makeRawStructure(2, 0, {item1, item2});
    EXPECT_NO_THROW(Structure s(raw));
}

TEST(TlvDecoderStructure, ConstructTooShort)
{
    std::vector<uint8_t> raw = {0x01, 0x02};
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, ConstructBadIdentifier)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(2, 0, {item1});
    // Corrupt identifier
    raw[0] = 0xFF;
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, ConstructSizeMismatch)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(2, 0, {item1});
    // Add extra byte to make size mismatch
    raw.push_back(0x00);
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, ConstructEmptyPayload)
{
    // Structure with no items (payload size = 0)
    auto raw = makeRawStructure(2, 0, {});
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, GetVersion)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(3, 5, {item1});
    Structure s(raw);
    auto [major, minor] = s.getVersion();
    EXPECT_EQ(major, 3);
    EXPECT_EQ(minor, 5);
}

TEST(TlvDecoderStructure, GetTypes)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto item2 = makeRawItem(types::Common::DeviceSerialNumber,
                             {0x01, 0x02, 0x03, 0x04});
    auto item3 = makeRawItem(types::Common::TokenType, {0x09, 0x00});
    auto raw = makeRawStructure(2, 0, {item1, item2, item3});
    Structure s(raw);
    auto types = s.getTypes();
    EXPECT_EQ(types.size(), 3u);
}

TEST(TlvDecoderStructure, GetItemFound)
{
    std::vector<uint8_t> serialData = {0x01, 0x02, 0x03, 0x04};
    auto item1 = makeRawItem(types::Common::DeviceSerialNumber, serialData);
    auto raw = makeRawStructure(2, 0, {item1});
    Structure s(raw);
    const auto& item = s.get(types::Common::DeviceSerialNumber);
    EXPECT_EQ(item.getType(), types::Common::DeviceSerialNumber);
    EXPECT_EQ(item.getRawValue(), serialData);
}

TEST(TlvDecoderStructure, GetItemNotFound)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(2, 0, {item1});
    Structure s(raw);
    EXPECT_THROW(s.get(types::Common::DeviceSerialNumber), std::runtime_error);
}

TEST(TlvDecoderStructure, DuplicateType)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto item2 = makeRawItem(types::Common::DeviceType, {0x02});
    auto raw = makeRawStructure(2, 0, {item1, item2});
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, MultipleItemsAccessByType)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto item2 =
        makeRawItem(types::Common::ChallengeNonce, {0xAA, 0xBB, 0xCC, 0xDD});
    auto item3 =
        makeRawItem(types::Common::FirmwareVersion, {0x01, 0x00, 0x00, 0x00});
    auto raw = makeRawStructure(2, 0, {item1, item2, item3});
    Structure s(raw);

    EXPECT_EQ(s.get(types::Common::DeviceType).getValue<uint8_t>(), 0x01);
    auto nonce = s.get(types::Common::ChallengeNonce).getRawValue();
    EXPECT_EQ(nonce.size(), 4u);
    EXPECT_EQ(nonce[0], 0xAA);
    EXPECT_EQ(s.get(types::Common::FirmwareVersion).getValue<uint32_t>(), 1u);
}

TEST(TlvDecoderStructure, DefaultConstructThenDecode)
{
    auto item1 = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(2, 0, {item1});
    Structure s;
    EXPECT_NO_THROW(s.decode(raw));
    EXPECT_EQ(s.get(types::Common::DeviceType).getValue<uint8_t>(), 0x01);
}

TEST(TlvDecoderStructure, GetTypesSingleItem)
{
    auto item = makeRawItem(types::Common::DeviceType, {0x01});
    auto raw = makeRawStructure(2, 0, {item});
    Structure s(raw);
    auto typeList = s.getTypes();
    ASSERT_EQ(typeList.size(), 1u);
    EXPECT_EQ(typeList[0], types::Common::DeviceType);
}

// ========================== Item with zero-length data
// ==========================

TEST(TlvDecoderItem, ZeroLengthData)
{
    auto raw = makeRawItem(0x0001, {});
    Item item(std::span{raw});
    EXPECT_EQ(item.getValueSize(), 0u);
    EXPECT_EQ(item.getTotalSize(), sizeof(ItemHeader));
    EXPECT_TRUE(item.getRawValue().empty());
}

// ========================== Structure with corrupt item data
// ==========================

TEST(TlvDecoderStructure, CorruptItemInPayload)
{
    // Build a structure with a valid header but corrupt item data inside
    StructureHeader hdr{};
    std::memcpy(hdr.identifier, TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    // Payload: 4 bytes, but item header says it needs more data
    std::vector<uint8_t> payload = {0x01, 0x00, 0xFF, 0x00}; // type=1, size=255
    hdr.size = htole32(static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> raw(sizeof(StructureHeader) + payload.size());
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    std::memcpy(raw.data() + sizeof(StructureHeader), payload.data(),
                payload.size());
    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

TEST(TlvDecoderStructure, CorruptSecondItemAfterValidFirstItem)
{
    auto validItem = makeRawItem(types::Common::DeviceType, {0x01});

    ItemHeader badHdr{};
    badHdr.type = htole16(types::Common::DeviceSerialNumber);
    badHdr.size = htole16(8);

    std::vector<uint8_t> payload(validItem.begin(), validItem.end());
    payload.resize(payload.size() + sizeof(ItemHeader));
    std::memcpy(payload.data() + validItem.size(), &badHdr, sizeof(ItemHeader));

    StructureHeader hdr{};
    std::memcpy(hdr.identifier, TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(static_cast<uint32_t>(payload.size()));

    std::vector<uint8_t> raw(sizeof(StructureHeader) + payload.size());
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    std::memcpy(raw.data() + sizeof(StructureHeader), payload.data(),
                payload.size());

    EXPECT_THROW(Structure s(raw), std::runtime_error);
}

// ========================== Error class tests ==========================

#include "../debug_token/tlv/error.h"

TEST(TlvError, KnownErrorCodes)
{
    using namespace debug_token;
    EXPECT_EQ(Error(InternalError).to_string(), "Internal error");
    EXPECT_EQ(Error(InvalidFormat).to_string(), "Invalid format");
    EXPECT_EQ(Error(SignatureVerificationFailed).to_string(),
              "Signature verification failed");
    EXPECT_EQ(Error(InvalidNonce).to_string(), "Invalid nonce");
    EXPECT_EQ(Error(InvalidLifecycleState).to_string(),
              "Invalid lifecycle state");
    EXPECT_EQ(Error(UnsupportedType).to_string(), "Unsupported type");
    EXPECT_EQ(Error(StorageError).to_string(), "Storage error");
    EXPECT_EQ(Error(RatchetCheckFailed).to_string(), "Ratchet check failed");
    EXPECT_EQ(Error(DeviceInternalError).to_string(), "Device internal error");
    EXPECT_EQ(Error(FeatureDisabled).to_string(), "Feature disabled");
    EXPECT_EQ(Error(FeatureDisabledByPolicy).to_string(),
              "Feature disabled by policy");
    EXPECT_EQ(Error(FwVersionMismatch).to_string(),
              "Firmware version mismatch");
    EXPECT_EQ(Error(InvalidSerialNumber).to_string(), "Invalid serial number");
    EXPECT_EQ(Error(InvalidPsid).to_string(), "Invalid PSID");
    EXPECT_EQ(Error(AlreadyInstalled).to_string(), "Token already installed");
    EXPECT_EQ(Error(NotInstalled).to_string(), "Token not installed");
    EXPECT_EQ(Error(TokenHashVerificationFailed).to_string(),
              "Token hash verification failed");
}

TEST(TlvError, UnknownErrorCode)
{
    debug_token::Error err(0xFFFF);
    auto msg = err.to_string();
    EXPECT_TRUE(msg.find("Unknown error code") != std::string::npos);
}

// ========================== Edge cases for Item constructor ==================

TEST(TlvItemEdge, ItemDataTooShort)
{
    // Create Item where header claims more data than available
    ItemHeader hdr;
    hdr.type = htole16(0x0001);
    hdr.size = htole16(100); // Claims 100 bytes of data
    std::vector<uint8_t> raw(sizeof(ItemHeader) +
                             5); // Only 5 bytes after header
    std::memcpy(raw.data(), &hdr, sizeof(ItemHeader));
    EXPECT_THROW(Item(std::span{raw.begin(), raw.end()}), std::runtime_error);
}

TEST(TlvItemEdge, ItemGetValueSizeMismatch)
{
    // Create an Item with 4 bytes of data, try to getValue<uint8_t>() → size
    // mismatch
    auto raw = makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04}); // 4 bytes
    Item item(std::span{raw.begin(), raw.end()});
    EXPECT_THROW(item.getValue<uint8_t>(), std::runtime_error);
}

TEST(TlvItemEdge, ItemGetValueUint16SizeMismatch)
{
    // 1 byte data, try getValue<uint16_t>
    auto raw = makeRawItem(0x0001, {0x01});
    Item item(std::span{raw.begin(), raw.end()});
    EXPECT_THROW(item.getValue<uint16_t>(), std::runtime_error);
}

// ========================== Edge cases for Structure constructor =============

TEST(TlvStructureEdge, StructureInvalidIdentifier)
{
    // Build a Structure binary with wrong magic identifier
    StructureHeader hdr;
    hdr.identifier[0] = 'X'; // Wrong
    hdr.identifier[1] = 'X';
    hdr.identifier[2] = 'X';
    hdr.identifier[3] = 'X';
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(0); // No payload
    std::vector<uint8_t> raw(sizeof(StructureHeader));
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    EXPECT_THROW((Structure(raw)), std::runtime_error);
}

TEST(TlvStructureEdge, StructureSizeMismatch)
{
    // Build Structure where header.size doesn't match actual data
    auto item = makeRawItem(0x0001, {0x42});
    StructureHeader hdr;
    std::memcpy(hdr.identifier, "TLV\x00", 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(100); // Claims 100 bytes payload, but only item.size()
    std::vector<uint8_t> raw(sizeof(StructureHeader) + item.size());
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    std::memcpy(raw.data() + sizeof(StructureHeader), item.data(), item.size());
    EXPECT_THROW((Structure(raw)), std::runtime_error);
}

TEST(TlvStructureEdge, StructureEmptyPayload)
{
    // Structure with no items → empty data
    StructureHeader hdr;
    std::memcpy(hdr.identifier, "TLV\x00", 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(0); // No payload
    std::vector<uint8_t> raw(sizeof(StructureHeader));
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    EXPECT_THROW((Structure(raw)), std::runtime_error); // empty data
}

TEST(TlvStructureEdge, StructureDuplicateType)
{
    // Two items with same type → duplicate error
    auto item1 = makeRawItem(0x0001, {0x42});
    auto item2 = makeRawItem(0x0001, {0x43}); // Same type!
    size_t payloadSize = item1.size() + item2.size();

    StructureHeader hdr;
    std::memcpy(hdr.identifier, "TLV\x00", 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));
    std::vector<uint8_t> raw(sizeof(StructureHeader) + payloadSize);
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    std::memcpy(raw.data() + sizeof(StructureHeader), item1.data(),
                item1.size());
    std::memcpy(raw.data() + sizeof(StructureHeader) + item1.size(),
                item2.data(), item2.size());
    EXPECT_THROW((Structure(raw)), std::runtime_error); // duplicate
}

TEST(TlvStructureEdge, StructureCorruptedItemInPayload)
{
    // Payload contains a corrupted item (claims more data than available)
    StructureHeader hdr;
    std::memcpy(hdr.identifier, "TLV\x00", 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    // Build a corrupt item header claiming 200 bytes
    ItemHeader itemHdr;
    itemHdr.type = htole16(0x0001);
    itemHdr.size = htole16(200);                 // Way more than available
    size_t payloadSize = sizeof(ItemHeader) + 2; // Only 2 bytes of data
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));
    std::vector<uint8_t> raw(sizeof(StructureHeader) + payloadSize);
    std::memcpy(raw.data(), &hdr, sizeof(StructureHeader));
    std::memcpy(raw.data() + sizeof(StructureHeader), &itemHdr,
                sizeof(ItemHeader));
    raw[sizeof(StructureHeader) + sizeof(ItemHeader)] = 0xAA;
    raw[sizeof(StructureHeader) + sizeof(ItemHeader) + 1] = 0xBB;
    EXPECT_THROW((Structure(raw)), std::runtime_error);
}
