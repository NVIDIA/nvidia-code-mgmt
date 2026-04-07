/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include <filesystem>
#include <optional>

namespace update_debug_token_hooks
{
bool useNextTokenHook = false;
bool nextTokenShouldReturnNull = false;
std::vector<uint8_t> nextTokenData;
std::vector<uint8_t> nextSerialNumber;

void reset()
{
    useNextTokenHook = false;
    nextTokenShouldReturnNull = false;
    nextTokenData.clear();
    nextSerialNumber.clear();
}

const TokenHeader* getNextDebugToken(std::ifstream& debugTokenPackage,
                                     uint32_t tokenOffset,
                                     std::vector<uint8_t>& tokenData,
                                     std::vector<uint8_t>& serialNumber)
{
    if (!useNextTokenHook)
    {
        TokenUtility helper;
        return helper.getNextDebugToken(debugTokenPackage, tokenOffset,
                                        tokenData, serialNumber);
    }

    tokenData = nextTokenData;
    serialNumber = nextSerialNumber;
    if (nextTokenShouldReturnNull || tokenData.size() < sizeof(TokenHeader))
    {
        return nullptr;
    }
    return reinterpret_cast<const TokenHeader*>(tokenData.data());
}
} // namespace update_debug_token_hooks

#define getNextDebugToken(...)                                                 \
    update_debug_token_hooks::getNextDebugToken(__VA_ARGS__)
#include "../debug_token/update_debug_token.cpp"
#undef getNextDebugToken
#include "../debug_token/nsm_debug_token.cpp"
#include "../debug_token/tlv/tlv_decoder.cpp"

using ::testing::NiceMock;

namespace
{

dbus::InterfaceMap makeMctpInterfaces(
    const std::optional<std::string>& uuid, const std::optional<uint8_t>& eid,
    const std::optional<std::string>& medium,
    const std::optional<std::string>& binding,
    const std::optional<std::string>& connectivity,
    const std::optional<std::vector<uint8_t>>& supportedTypes)
{
    dbus::InterfaceMap interfaces;

    if (eid || medium || supportedTypes)
    {
        dbus::PropertyMap endpointProps;
        if (eid)
        {
            endpointProps["EID"] = *eid;
        }
        if (medium)
        {
            endpointProps["MediumType"] = *medium;
        }
        if (supportedTypes)
        {
            endpointProps["SupportedMessageTypes"] = *supportedTypes;
        }
        interfaces[mctpEndpointIntfName] = endpointProps;
    }

    if (uuid)
    {
        interfaces[uuidEndpointIntfName] = dbus::PropertyMap{{"UUID", *uuid}};
    }

    if (binding)
    {
        interfaces[mctpBindingIntfName] =
            dbus::PropertyMap{{"BindingType", *binding}};
    }

    if (connectivity)
    {
        interfaces[mctpEndpointEnableIntfName] =
            dbus::PropertyMap{{"Connectivity", *connectivity}};
    }

    return interfaces;
}

dbus::InterfaceMap makePldmInterfaces(const std::string& uuid,
                                      const std::string& serial)
{
    return dbus::InterfaceMap{
        {uuidEndpointIntfName, dbus::PropertyMap{{"UUID", uuid}}},
        {pldmInventoryIntfName, dbus::PropertyMap{{"SerialNumber", serial}}},
    };
}

class UpdateDebugTokenIncludedTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    UpdateDebugToken udt{bus};

    void SetUp() override
    {
        update_debug_token_hooks::reset();
    }

    void TearDown() override
    {
        update_debug_token_hooks::reset();
    }
};

} // namespace

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoAllAssignmentsCovered)
{
    auto interfaces = makeMctpInterfaces(
        std::string("gpu-a"), uint8_t{42},
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe"),
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"),
        std::string("Available"),
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA});

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 42);
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    EXPECT_EQ(info.binding,
              "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    EXPECT_EQ(info.supportedMsgTypes,
              (SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}));
    EXPECT_TRUE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoConnectivityUnavailableBranch)
{
    auto interfaces = makeMctpInterfaces(
        std::string("gpu-b"), uint8_t{43},
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe"),
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"),
        std::string("Unavailable"),
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA});

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 43);
    EXPECT_FALSE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoMissingConnectivityBranch)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = dbus::PropertyMap{
        {"EID", uint8_t{44}},
        {"SupportedMessageTypes",
         SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
    };
    interfaces[mctpEndpointEnableIntfName] = dbus::PropertyMap{};

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 44);
    EXPECT_FALSE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoMissingEnableInterfaceBranch)
{
    auto interfaces = makeMctpInterfaces(
        std::string("gpu-c"), uint8_t{45},
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus"),
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"),
        std::nullopt, SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA});

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 45);
    EXPECT_EQ(info.medium,
              "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus");
    EXPECT_FALSE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest,
       FetchEidInfoBindingInterfaceWithoutTypeBranch)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = dbus::PropertyMap{
        {"EID", uint8_t{46}},
        {"SupportedMessageTypes",
         SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
    };
    interfaces[mctpBindingIntfName] = dbus::PropertyMap{};
    interfaces[mctpEndpointEnableIntfName] =
        dbus::PropertyMap{{"Connectivity", std::string("Available")}};

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 46);
    EXPECT_TRUE(info.binding.empty());
    EXPECT_TRUE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoMissingEndpointValuesBranch)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = dbus::PropertyMap{
        {"SupportedMessageTypes",
         SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
    };
    interfaces[mctpEndpointEnableIntfName] =
        dbus::PropertyMap{{"Connectivity", std::string("Available")}};

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_TRUE(info.enabled);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoWrongSupportedTypeThrows)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = dbus::PropertyMap{
        {"EID", uint8_t{47}},
        {"SupportedMessageTypes", std::string("invalid")},
    };

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(UpdateDebugTokenIncludedTest, FetchEidInfoWrongConnectivityTypeThrows)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointIntfName] = dbus::PropertyMap{
        {"EID", uint8_t{48}},
        {"SupportedMessageTypes",
         SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
    };
    interfaces[mctpEndpointEnableIntfName] =
        dbus::PropertyMap{{"Connectivity", uint8_t{1}}};

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(UpdateDebugTokenIncludedTest,
       UpdateDeviceMapCoversInsertAndDuplicateBranches)
{
    udt.mctpInfo["uuid-first"] =
        MctpEidInfo{42,
                    "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                    "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                    {mctpTypeSPDM, mctpTypeVDMIANA},
                    true};

    udt.updateDeviceMap(makePldmInterfaces("uuid-first", "SN-FIRST"),
                        "GPU_FIRST");
    udt.updateDeviceMap(makePldmInterfaces("uuid-first", "SN-SECOND"),
                        "GPU_SECOND");
    udt.updateDeviceMap(makePldmInterfaces("uuid-missing", "SN-MISSING"),
                        "GPU_MISSING");
    udt.updateDeviceMap(
        dbus::InterfaceMap{
            {uuidEndpointIntfName,
             dbus::PropertyMap{{"UUID", std::string("")}}},
            {pldmInventoryIntfName,
             dbus::PropertyMap{{"SerialNumber", std::string("SN-EMPTY")}}}},
        "GPU_EMPTY");

    ASSERT_EQ(udt.devices.size(), 1u);
    EXPECT_EQ(udt.devices.at(42), "SN-FIRST");
    EXPECT_EQ(udt.deviceNameMap.at(42), "GPU_FIRST");
}

TEST_F(UpdateDebugTokenIncludedTest, UpdateDeviceMapWrongSerialTypeThrows)
{
    udt.mctpInfo["uuid-serial-type"] =
        MctpEidInfo{49,
                    "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                    "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                    {mctpTypeSPDM, mctpTypeVDMIANA},
                    true};

    dbus::InterfaceMap interfaces{
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-serial-type")}}},
        {pldmInventoryIntfName,
         dbus::PropertyMap{{"SerialNumber", uint8_t{9}}}},
    };

    EXPECT_THROW(udt.updateDeviceMap(interfaces, "GPU_BAD_SERIAL"),
                 std::bad_variant_access);
}

TEST_F(UpdateDebugTokenIncludedTest,
       UpdateDeviceMapMissingUuidOrSerialSkipsInsert)
{
    udt.mctpInfo["uuid-known"] =
        MctpEidInfo{50,
                    "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                    "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                    {mctpTypeSPDM, mctpTypeVDMIANA},
                    true};

    udt.updateDeviceMap(
        dbus::InterfaceMap{
            {uuidEndpointIntfName, dbus::PropertyMap{}},
            {pldmInventoryIntfName,
             dbus::PropertyMap{{"SerialNumber", std::string("SN-EMPTY-UUID")}}},
        },
        "GPU_EMPTY_UUID");

    udt.updateDeviceMap(
        dbus::InterfaceMap{
            {uuidEndpointIntfName,
             dbus::PropertyMap{{"UUID", std::string("uuid-known")}}},
            {pldmInventoryIntfName, dbus::PropertyMap{}},
        },
        "GPU_EMPTY_SERIAL");

    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(UpdateDebugTokenIncludedTest, UpdateTokenMapInvalidTokenSizeBranch)
{
    auto tmpPath = std::filesystem::temp_directory_path() /
                   "test_update_token_size_mismatch_direct.bin";

    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = FileTypeDebugToken;
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }

    update_debug_token_hooks::useNextTokenHook = true;
    update_debug_token_hooks::nextSerialNumber =
        std::vector<uint8_t>(tokenSerialNumberSizeDefault, 0x11);
    update_debug_token_hooks::nextTokenData.resize(sizeof(TokenHeader) + 8);
    auto* token = reinterpret_cast<TokenHeader*>(
        update_debug_token_hooks::nextTokenData.data());
    std::memcpy(token->identifier, "CRDT", 4);
    token->structSize = sizeof(TokenHeader) + 32;

    TokenMap tokens;
    EXPECT_EQ(udt.updateTokenMap(tmpPath.string(), tokens), -1);
    EXPECT_TRUE(tokens.empty());

    std::filesystem::remove(tmpPath);
}
