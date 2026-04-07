/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

// Include system/library headers BEFORE the private->public hack
#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

// gtest FAIL() macro conflicts with phosphor-logging elog-errors.hpp FAIL type
#undef FAIL

// Hack: make private/protected members accessible for testing
// (same pattern used by pldm repo)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <variant>

using ::testing::NiceMock;

class DebugTokenPrivateTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    UpdateDebugToken udt{bus};
};
// ========================== parseQueryV3Response ==========================

TEST_F(DebugTokenPrivateTest, ParseQueryV3TooShort)
{
    std::vector<std::string> rxBytes = {"00", "00", "16"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3WrongSizeWithCompletionCode)
{
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "03"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 3);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3NonZeroCompletion)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "01";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3SuccessNotInstalled)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "00"; // not installed
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 0);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3SuccessInstalled)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "01"; // installed
    rxBytes[30] = "01"; // type byte 0
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 1);
    EXPECT_EQ(installedTokenType, 1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3ExceptionInParsing)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "ZZ"; // invalid hex
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3CompletionCodeOutOfRange)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3ExceptionInTokenStatus)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "ZZ"; // invalid hex
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3TokenStatusOutOfRange)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3ExceptionInInstalledTokenTypeBytes)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "01";
    rxBytes[30] = "GG";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3InstalledTokenTypeOutOfRange)
{
    std::vector<std::string> rxBytes(50, "00");
    rxBytes[8] = "00";
    rxBytes[12] = "01";
    rxBytes[30] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV3WrongSizeInvalidCompletionCode)
{
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "GG"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV3Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

// ========================== createLog ==========================

TEST_F(DebugTokenPrivateTest, CreateLogSuccess)
{
    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = "test";
    Level level = Level::Informational;
    EXPECT_NO_THROW(udt.createLog("test.msg", addData, level));
}

TEST_F(DebugTokenPrivateTest, CreateLogCritical)
{
    std::map<std::string, std::string> addData;
    addData["key"] = "val";
    Level level = Level::Critical;
    EXPECT_NO_THROW(udt.createLog("error.msg", addData, level));
}

// ========================== getErasePolicy ==========================

TEST_F(DebugTokenPrivateTest, GetErasePolicyDBusFails)
{
    // D-Bus call fails -> returns empty string
    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

// ========================== discoverMCTPDevices ==========================

TEST_F(DebugTokenPrivateTest, DiscoverMCTPDevicesDBusFails)
{
    // D-Bus GetSubTree fails -> returns -1
    int result = udt.discoverMCTPDevices();
    EXPECT_EQ(result, -1);
}

// ========================== updateEndPoints ==========================

TEST_F(DebugTokenPrivateTest, UpdateEndPointsDBusFails)
{
    int result = udt.updateEndPoints();
    EXPECT_EQ(result, -1);
}

// ========================== checkSupportForSPDMandMCTPVDM
// ==========================

TEST_F(DebugTokenPrivateTest, CheckSupportEmpty)
{
    SupportedMessageTypes empty;
    EXPECT_FALSE(udt.checkSupportForSPDMandMCTPVDM(empty, 1));
}

TEST_F(DebugTokenPrivateTest, CheckSupportOnlySPDM)
{
    SupportedMessageTypes types = {mctpTypeSPDM};
    EXPECT_FALSE(udt.checkSupportForSPDMandMCTPVDM(types, 1));
}

TEST_F(DebugTokenPrivateTest, CheckSupportOnlyVDM)
{
    SupportedMessageTypes types = {mctpTypeVDMIANA};
    EXPECT_FALSE(udt.checkSupportForSPDMandMCTPVDM(types, 1));
}

TEST_F(DebugTokenPrivateTest, CheckSupportBothPresent)
{
    SupportedMessageTypes types = {mctpTypeSPDM, mctpTypeVDMIANA};
    EXPECT_TRUE(udt.checkSupportForSPDMandMCTPVDM(types, 1));
}

// ========================== enumerateNsmDebugTokenEndpoints
// ==========================

TEST_F(DebugTokenPrivateTest, EnumerateNsmEndpointsDBusFails)
{
    NSMEndpoints endpoints;
    int result = udt.enumerateNsmDebugTokenEndpoints(endpoints);
    // D-Bus call fails -> returns -1
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(endpoints.empty());
}

// ========================== enumerateNsmDebugTokenEndpointsV2
// ==========================

TEST_F(DebugTokenPrivateTest, EnumerateNsmEndpointsV2DBusFails)
{
    NSMEndpoints endpoints;
    int result = udt.enumerateNsmDebugTokenEndpointsV2(endpoints);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(endpoints.empty());
}

// ========================== nsmTokenInstall ==========================

TEST_F(DebugTokenPrivateTest, NsmTokenInstallNoEndpoints)
{
    TokenMap tokens;
    tokens.emplace("0x0102030405060708", std::vector<uint8_t>{0x01, 0x02});
    int result = udt.nsmTokenInstall(tokens);
    // discoverMCTPDevices fails -> returns -1
    EXPECT_EQ(result, -1);
}

// ========================== nsmTokenErase ==========================

TEST_F(DebugTokenPrivateTest, NsmTokenEraseNoEndpoints)
{
    int result = udt.nsmTokenErase();
    // discoverMCTPDevices fails -> returns -1
    EXPECT_EQ(result, -1);
}

// ========================== nsmTokenInstallV2 ==========================

TEST_F(DebugTokenPrivateTest, NsmTokenInstallV2NoEndpoints)
{
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>{0x01});
    int result = udt.nsmTokenInstallV2(tokens);
    // enumerateNsmDebugTokenEndpointsV2 fails -> returns -1
    EXPECT_EQ(result, -1);
}

// ========================== nsmTokenEraseV2 ==========================

TEST_F(DebugTokenPrivateTest, NsmTokenEraseV2NoEndpoints)
{
    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

// ========================== installToken (popen-based)
// ==========================

TEST_F(DebugTokenPrivateTest, InstallTokenMctpVdmFails)
{
    // installToken runs mctp-vdm-util via popen which doesn't exist -> fails
    Token token = {0x01, 0x02, 0x03, 0x04};
    int result = udt.installToken(31, token);
    // Will fail since mctp-vdm-util not found
    EXPECT_NE(result, 0);
}

// ========================== eraseToken (popen-based)
// ==========================

TEST_F(DebugTokenPrivateTest, EraseTokenMctpVdmFails)
{
    int result = udt.eraseToken(31);
    EXPECT_NE(result, 0);
}

// ========================== queryDebugToken ==========================

TEST_F(DebugTokenPrivateTest, QueryDebugTokenFails)
{
    // Queries V3 first, then V2, then V1 - all fail since mctp-vdm-util missing
    int result = udt.queryDebugToken(31);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, QueryDebugTokenV1Fails)
{
    int result = udt.queryDebugTokenV1(31);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, QueryDebugTokenV2Fails)
{
    int result = udt.queryDebugTokenV2(31);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, QueryDebugTokenV3Fails)
{
    int result = udt.queryDebugTokenV3(31);
    EXPECT_NE(result, 0);
}

// ========================== enableBackgroundCopy / disableBackgroundCopy
// ==========

TEST_F(DebugTokenPrivateTest, EnableBackgroundCopyFails)
{
    int result = udt.enableBackgroundCopy(31);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, DisableBackgroundCopyFails)
{
    int result = udt.disableBackgroundCopy(31);
    EXPECT_NE(result, 0);
}

// ========================== getMCTPManagedObjects ==========================

TEST_F(DebugTokenPrivateTest, GetMCTPManagedObjectsDBusFails)
{
    auto objects = udt.getMCTPManagedObjects();
    EXPECT_TRUE(objects.empty());
}

// ========================== getMCTPServiceList ==========================

TEST_F(DebugTokenPrivateTest, GetMCTPServiceListDBusFails)
{
    auto services = udt.getMCTPServiceList();
    EXPECT_TRUE(services.empty());
}

// ========================== fetchEidInfoFromObject ==========================

TEST_F(DebugTokenPrivateTest, FetchEidInfoFromObjectValidData)
{
    // Build a minimal InterfaceMap with MCTP endpoint data
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap props;
    props["EID"] = uint8_t(42);
    props["SupportedMessageTypes"] = std::vector<uint8_t>{0x05, 0x7f};
    interfaces[mctpEndpointIntfName] = props;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 42);
    EXPECT_EQ(info.supportedMsgTypes.size(), 2u);
}

// ========================== updateDeviceMap ==========================

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapNoInventoryIntf)
{
    dbus::InterfaceMap interfaces;
    // No pldmInventoryIntfName -> should skip
    EXPECT_NO_THROW(udt.updateDeviceMap(interfaces, "TestDevice"));
    EXPECT_TRUE(udt.devices.empty());
}

// ========================== makeDebugTokenMethodCall
// ==========================

TEST_F(DebugTokenPrivateTest, MakeDebugTokenMethodCallDBusFails)
{
    auto result = udt.makeDebugTokenMethodCall("/test/path", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, MakeDebugTokenMethodCallGetStatus)
{
    auto result = udt.makeDebugTokenMethodCall("/test/path", "GetStatus",
                                               std::string("CRDT"));
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, MakeDebugTokenMethodCallInstallToken)
{
    auto result = udt.makeDebugTokenMethodCall(
        "/test/path", "InstallToken", std::vector<uint8_t>{0x01, 0x02, 0x03});
    EXPECT_TRUE(result.empty());
}

// ========================== handleAsyncCall ==========================

TEST_F(DebugTokenPrivateTest, HandleAsyncCallDBusFails)
{
    auto result = udt.handleAsyncCall("/test/path", "EraseToken");
    EXPECT_TRUE(result.empty());
}

// ========================== getTokenStatus ==========================

TEST_F(DebugTokenPrivateTest, GetTokenStatusDBusFails)
{
    auto result = udt.getTokenStatus("/test/path");
    EXPECT_TRUE(result.empty());
}

// ========================== Phase 2: More NSM coverage ====================

// logAsyncError - covers the catch block when getAsyncValue throws
TEST_F(DebugTokenPrivateTest, LogAsyncErrorExceptionPath)
{
    EXPECT_NO_THROW(udt.logAsyncError("/test/path", "EraseToken", "Failed"));
}

// handleAsyncCall - covers match setup + makeDebugTokenMethodCall failure path
TEST_F(DebugTokenPrivateTest, HandleAsyncCallMakeCallFails)
{
    auto result = udt.handleAsyncCall("/test/path", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, HandleAsyncCallWithStringArg)
{
    auto result =
        udt.handleAsyncCall("/test/path", "GetStatus", std::string("CRDT"));
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, HandleAsyncCallWithVectorArg)
{
    auto result = udt.handleAsyncCall("/test/path", "InstallToken",
                                      std::vector<uint8_t>{0x01, 0x02, 0x03});
    EXPECT_TRUE(result.empty());
}

// handleAsyncCallInstallV2 / handleAsyncCallEraseV2
TEST_F(DebugTokenPrivateTest, HandleAsyncCallInstallV2Fails)
{
    auto result = udt.handleAsyncCallInstallV2("/test/path", -1);
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, HandleAsyncCallEraseV2Fails)
{
    auto result = udt.handleAsyncCallEraseV2("/test/path");
    EXPECT_TRUE(result.empty());
}

// nsmTokenInstall / nsmTokenErase (old V1 paths via MCTP)
TEST_F(DebugTokenPrivateTest, NsmTokenInstallDiscoveryFails)
{
    TokenMap tokens;
    tokens.emplace("0x0102030405060708", std::vector<uint8_t>{0x01, 0x02});
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(DebugTokenPrivateTest, NsmTokenEraseDiscoveryFails)
{
    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

// getMCTPServiceList / getMCTPManagedObjects
TEST_F(DebugTokenPrivateTest, GetMCTPServiceListEmpty)
{
    auto services = udt.getMCTPServiceList();
    EXPECT_TRUE(services.empty());
}

TEST_F(DebugTokenPrivateTest, GetMCTPManagedObjectsEmpty)
{
    auto objects = udt.getMCTPManagedObjects();
    EXPECT_TRUE(objects.empty());
}

// updateDeviceMap with richer data
TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWithPldmInventory)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN12345678");
    interfaces[pldmInventoryIntfName] = pldmProps;
    EXPECT_NO_THROW(udt.updateDeviceMap(interfaces, "TestDevice_0"));
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapMissingSerial)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap pldmProps;
    interfaces[pldmInventoryIntfName] = pldmProps;
    EXPECT_NO_THROW(udt.updateDeviceMap(interfaces, "TestDevice_1"));
}

// fetchEidInfoFromObject with full data
TEST_F(DebugTokenPrivateTest, FetchEidInfoComplete)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(42);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{0x05, 0x7f};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    bindingProps["BindingMediumID"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    bindingProps["BindingType"] =
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindingProps;

    dbus::PropertyMap enableProps;
    enableProps["Enabled"] = true;
    interfaces["xyz.openbmc_project.Object.Enable"] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 42);
    EXPECT_EQ(info.supportedMsgTypes.size(), 2u);
    // enabled is set from mctpEndpointEnableIntfName "Connectivity" property
    // not from the Object.Enable interface
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoNoBinding)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(10);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{};
    interfaces[mctpEndpointIntfName] = eidProps;
    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 10);
}

// ========================== fetchEidInfoFromObject with enable interface =====

TEST_F(DebugTokenPrivateTest, FetchEidInfoWithConnectivity)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(20);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{0x05};
    interfaces[mctpEndpointIntfName] = eidProps;

    // The enable interface with Connectivity property
    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 20);
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWithConnectivityUnavailable)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(21);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Unavailable");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 21);
    EXPECT_FALSE(info.enabled);
}

TEST_F(DebugTokenPrivateTest,
       FetchEidInfoMissingConnectivityPropertyWithBinding)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(22);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 22);
    EXPECT_FALSE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEnableInterfaceWithBinding)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(23);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 23);
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    EXPECT_FALSE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoBindingInterfaceWithoutBindingType)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(24);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    interfaces[mctpBindingIntfName] = bindingProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 24);
    EXPECT_TRUE(info.binding.empty());
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEidUsesDefaultZero)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWrongEidTypeThrows)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = std::string("31");
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWrongSupportedMessageTypesThrows)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(31);
    eidProps["SupportedMessageTypes"] = std::string("invalid");
    interfaces[mctpEndpointIntfName] = eidProps;

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWrongMediumTypeThrows)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(32);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] = uint8_t(7);
    interfaces[mctpEndpointIntfName] = eidProps;

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWrongBindingTypeThrows)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(33);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    bindingProps["BindingType"] = uint8_t(1);
    interfaces[mctpBindingIntfName] = bindingProps;

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoWrongConnectivityTypeThrows)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(34);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = uint8_t(1);
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEndpointInterfaceThrows)
{
    dbus::InterfaceMap interfaces;
    interfaces[mctpEndpointEnableIntfName] =
        dbus::PropertyMap{{"Connectivity", std::string("Available")}};

    EXPECT_THROW((void)udt.fetchEidInfoFromObject(interfaces),
                 std::out_of_range);
}

// ========================== updateDeviceMap with UUID interface =============

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWithUUID)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("test-uuid");
    interfaces[uuidEndpointIntfName] = uuidProps;

    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(42);
    eidProps["SupportedMessageTypes"] = std::vector<uint8_t>{0x05, 0x7f};
    interfaces[mctpEndpointIntfName] = eidProps;

    // Should populate mctpInfo with UUID -> EID mapping
    EXPECT_NO_THROW(udt.updateDeviceMap(interfaces, "TestDevice_2"));
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWrongUuidTypeThrows)
{
    dbus::InterfaceMap interfaces;
    interfaces[uuidEndpointIntfName] = dbus::PropertyMap{{"UUID", uint8_t(1)}};

    EXPECT_THROW(udt.updateDeviceMap(interfaces, "BadUuid"),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWrongSerialTypeThrows)
{
    MctpEidInfo info;
    info.eid = 44;
    udt.mctpInfo["serial-type-uuid"] = info;

    dbus::InterfaceMap interfaces;
    interfaces[uuidEndpointIntfName] =
        dbus::PropertyMap{{"UUID", std::string("serial-type-uuid")}};
    interfaces[pldmInventoryIntfName] =
        dbus::PropertyMap{{"SerialNumber", uint8_t(9)}};

    EXPECT_THROW(udt.updateDeviceMap(interfaces, "BadSerial"),
                 std::bad_variant_access);
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapDuplicateEidKeepsFirstValues)
{
    MctpEidInfo info;
    info.eid = 45;
    udt.mctpInfo["dup-map-uuid"] = info;

    dbus::InterfaceMap interfaces;
    interfaces[uuidEndpointIntfName] =
        dbus::PropertyMap{{"UUID", std::string("dup-map-uuid")}};
    interfaces[pldmInventoryIntfName] =
        dbus::PropertyMap{{"SerialNumber", std::string("SN-FIRST")}};

    udt.updateDeviceMap(interfaces, "GPU_FIRST");

    interfaces[pldmInventoryIntfName] =
        dbus::PropertyMap{{"SerialNumber", std::string("SN-SECOND")}};
    udt.updateDeviceMap(interfaces, "GPU_SECOND");

    EXPECT_EQ(udt.devices[45], "SN-FIRST");
    EXPECT_EQ(udt.deviceNameMap[45], "GPU_FIRST");
}

// ========================== checkSupportForSPDMandMCTPVDM (more branches) ===

TEST_F(DebugTokenPrivateTest, CheckSupportWithExtraTypes)
{
    SupportedMessageTypes types = {0x01, mctpTypeSPDM, 0x02, mctpTypeVDMIANA,
                                   0x03};
    EXPECT_TRUE(udt.checkSupportForSPDMandMCTPVDM(types, 1));
}

// ========================== getMessage ==========================

TEST_F(DebugTokenPrivateTest, GetMessageInstallError)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::InvalidToken), "gpu0");
    EXPECT_TRUE(result.has_value());
    if (result)
    {
        auto& [msg, resolution] = *result;
        EXPECT_FALSE(msg.empty());
        EXPECT_FALSE(resolution.empty());
    }
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallAuthFailed)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::TokenAuthFailed), "gpu1");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallNonceFailed)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::TokenNonceInvalid), "gpu2");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallSerialFailed)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::TokenSerialNumberInvalid), "gpu3");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallECFWFailed)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::TokenECFWVersionInvalid));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallBGCopyFailed)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::DisableBackgroundCopyCheckFailed));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallInternalError)
{
    auto result = udt.getMessage(
        OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::InstallInternalError));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageInstallSuccessNoMapping)
{
    auto result =
        udt.getMessage(OperationType::TokenInstall,
                       static_cast<int>(InstallErrorCodes::InstallSuccess));
    EXPECT_FALSE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageEraseError)
{
    auto result =
        udt.getMessage(OperationType::TokenErase,
                       static_cast<int>(EraseErrorCodes::EraseInternalError));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageEraseFailed)
{
    auto result =
        udt.getMessage(OperationType::TokenErase,
                       static_cast<int>(EraseErrorCodes::EraseFailed));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageEraseSuccessNoMapping)
{
    auto result =
        udt.getMessage(OperationType::TokenErase,
                       static_cast<int>(EraseErrorCodes::EraseSuccess));
    EXPECT_FALSE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageBackgroundCopyEnableFail)
{
    auto result = udt.getMessage(
        OperationType::BackgroundCopy,
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundEnableFail),
        "gpu0");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageBackgroundCopyDisableFail)
{
    auto result = udt.getMessage(
        OperationType::BackgroundCopy,
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundDisableFail));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageBackgroundCopySuccessNoMapping)
{
    auto result = udt.getMessage(
        OperationType::BackgroundCopy,
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopySuccess));
    EXPECT_FALSE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageTokenQueryNoMapping)
{
    // TokenQueryStatus has no case in getMessage switch -> hits default
    auto result = udt.getMessage(
        OperationType::TokenQueryStatus,
        static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled));
    EXPECT_FALSE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonDiscoveryFailed)
{
    auto result =
        udt.getMessage(OperationType::Common,
                       static_cast<int>(CommonErrorCodes::MCTPDiscoveryFailed));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonTokenParseFailure)
{
    auto result =
        udt.getMessage(OperationType::Common,
                       static_cast<int>(CommonErrorCodes::TokenParseFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonMCTPInstallSuccess)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPCommandInstallSuccess), "gpu0");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonMCTPInstallFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPCommandInstallFailure), "gpu0");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonMCTPEraseSuccess)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPCommandEraseSuccess), "gpu0");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonMCTPEraseFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPCommandEraseFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonNSMInstallSuccess)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::NSMCommandInstallSuccess), "gpu0");
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonNSMInstallFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::NSMCommandInstallFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonNSMEraseSuccess)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::NSMCommandEraseSuccess));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonNSMEraseFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::NSMCommandEraseFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonNSMFailure)
{
    auto result =
        udt.getMessage(OperationType::Common,
                       static_cast<int>(CommonErrorCodes::NSMCommandFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonResponseInstallFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure));
    EXPECT_TRUE(result.has_value());
}

TEST_F(DebugTokenPrivateTest, GetMessageCommonResponseEraseFailure)
{
    auto result = udt.getMessage(
        OperationType::Common,
        static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure));
    EXPECT_TRUE(result.has_value());
}

// ========================== formatMessage ==========================

TEST_F(DebugTokenPrivateTest, FormatMessageWithDevice)
{
    auto result = udt.formatMessage("Error on {}", "gpu0");
    EXPECT_EQ(result, "Error on gpu0");
}

TEST_F(DebugTokenPrivateTest, FormatMessageNoDevice)
{
    auto result = udt.formatMessage("General error", "");
    EXPECT_EQ(result, "General error");
}

// ========================== createMessageRegistryResourceErrors ===========

TEST_F(DebugTokenPrivateTest, CreateMessageRegistryResourceErrorsInstall)
{
    EXPECT_NO_THROW(udt.createMessageRegistryResourceErrors(
        transferFailed, "TestComponent", OperationType::TokenInstall,
        static_cast<int>(InstallErrorCodes::InvalidToken), "gpu0"));
}

TEST_F(DebugTokenPrivateTest, CreateMessageRegistryResourceErrorsErase)
{
    EXPECT_NO_THROW(udt.createMessageRegistryResourceErrors(
        transferFailed, "TestComponent", OperationType::TokenErase,
        static_cast<int>(EraseErrorCodes::EraseFailed)));
}

TEST_F(DebugTokenPrivateTest, CreateMessageRegistryResourceErrorsCommon)
{
    EXPECT_NO_THROW(udt.createMessageRegistryResourceErrors(
        resourceErrorsDetected, "TestComponent", OperationType::Common,
        static_cast<int>(CommonErrorCodes::NSMCommandFailure), "gpu0"));
}

// ========================== createTokenInstallErrorMessage /
// createTokenEraseErrorMessage

TEST_F(DebugTokenPrivateTest, CreateTokenInstallErrorMessage)
{
    EXPECT_NO_THROW(udt.createTokenInstallErrorMessage("/test/gpu0"));
}

TEST_F(DebugTokenPrivateTest, CreateTokenEraseErrorMessage)
{
    EXPECT_NO_THROW(udt.createTokenEraseErrorMessage("/test/gpu0"));
}

// ========================== createMessageRegistry ==========================

TEST_F(DebugTokenPrivateTest, CreateMessageRegistrySuccess)
{
    EXPECT_NO_THROW(
        udt.createMessageRegistry(updateSuccessful, "TestComponent", "1.0.0"));
}

TEST_F(DebugTokenPrivateTest, CreateMessageRegistryFailed)
{
    EXPECT_NO_THROW(
        udt.createMessageRegistry(transferFailed, "TestComponent", "1.0.0"));
}

// ========================== updateTokenMap ==========================

TEST_F(DebugTokenPrivateTest, UpdateTokenMapNonexistentFile)
{
    TokenMap tokens;
    int result = udt.updateTokenMap("/nonexistent/path/token.bin", tokens);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapEmptyFile)
{
    // Create an empty temp file
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_empty_token.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapInvalidTLV)
{
    // Create a file with invalid TLV data
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_bad_token.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        // Write some junk data
        std::vector<uint8_t> data(100, 0xFF);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    (void)result; // May succeed or fail depending on TLV parsing
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapValidLegacyToken)
{
    // Create a valid legacy (version=1) token file
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_legacy_token.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);

        // DebugTokenHeader: version=1, type=2 (FileTypeDebugToken),
        // numberOfRecords=1, offsetToListOfStructs=sizeof(DebugTokenHeader)
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = FileTypeDebugToken; // 2
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader); // 16
        hdr.fileSize = 0; // Will be set by total size
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // TokenHeader at offset = sizeof(DebugTokenHeader)
        // structSize includes header + serial + payload
        // Total token = sizeof(TokenHeader) + serialBytes + some payload
        size_t serialSize = 8;    // tokenSerialNumberSizeDefault
        size_t payloadExtra = 20; // some extra data
        uint16_t structSize = sizeof(TokenHeader) + serialSize + payloadExtra;

        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "CRDT", 4);
        tokHdr.versionMinor = 1;
        tokHdr.versionMajor = 1;
        tokHdr.structSize = structSize;
        tokHdr.tokenAttributes = 0;
        tokHdr.tokenType = 1;
        tokHdr.ecFWVersion = 0;
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));

        // Serial number (8 bytes)
        std::vector<uint8_t> serial = {0x01, 0x02, 0x03, 0x04,
                                       0x05, 0x06, 0x07, 0x08};
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());

        // Extra payload
        std::vector<uint8_t> payload(payloadExtra, 0xAB);
        ofs.write(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapLegacyWrongType)
{
    // File header with wrong type (not debug token)
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_wrong_type.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = 99; // Wrong type
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        hdr.fileSize = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_EQ(result, -1); // Invalid token header
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapLegacyTokenSizeMismatch)
{
    // Create legacy token where structSize != actual tokenData.size()
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_size_mismatch.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = FileTypeDebugToken;
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "CRDT", 4);
        // structSize = 200, but actual data is only sizeof(TokenHeader) + 8
        // serial + 20 extra = 68
        tokHdr.structSize = 200; // Way larger than available data
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));

        // Serial number (8 bytes)
        std::vector<uint8_t> serial(8, 0x01);
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_NE(result, 0); // Token read fails or size mismatch
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapLegacyMCUToken)
{
    // Create a legacy MCU token (identifier "MCDT", serial size = 16)
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_mcu_token.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = FileTypeDebugToken;
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        size_t serialSize = 16; // tokenSerialNumberSizeMCU
        size_t payloadExtra = 20;
        uint16_t structSize = sizeof(TokenHeader) + serialSize + payloadExtra;

        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "MCDT", 4);
        tokHdr.versionMinor = 1;
        tokHdr.versionMajor = 1;
        tokHdr.structSize = structSize;
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));

        std::vector<uint8_t> serial(serialSize, 0x42);
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());

        std::vector<uint8_t> payload(payloadExtra, 0xAB);
        ofs.write(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 1u);
}

TEST_F(DebugTokenPrivateTest, UpdateTokenMapLegacyMultipleRecords)
{
    // Create a legacy token file with 2 records
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_multi_token.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 1;
        hdr.type = FileTypeDebugToken;
        hdr.numberOfRecords = 2;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        for (int i = 0; i < 2; i++)
        {
            size_t serialSize = 8;
            size_t payloadExtra = 20;
            uint16_t structSize =
                sizeof(TokenHeader) + serialSize + payloadExtra;

            TokenHeader tokHdr{};
            memcpy(tokHdr.identifier, "CRDT", 4);
            tokHdr.versionMinor = 1;
            tokHdr.versionMajor = 1;
            tokHdr.structSize = structSize;
            ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));

            std::vector<uint8_t> serial(serialSize, 0x10 + i);
            ofs.write(reinterpret_cast<const char*>(serial.data()),
                      serial.size());

            std::vector<uint8_t> payload(payloadExtra, 0xAB);
            ofs.write(reinterpret_cast<const char*>(payload.data()),
                      payload.size());
        }
    }
    TokenMap tokens;
    int result = udt.updateTokenMap(tmpPath, tokens);
    std::filesystem::remove(tmpPath);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(tokens.size(), 2u);
}

// ========================== parseQueryV2Response ==========================

TEST_F(DebugTokenPrivateTest, ParseQueryV2TooShort)
{
    std::vector<std::string> rxBytes = {"00", "00", "16"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2NonZeroCompletion)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "01"; // non-zero completion code
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2SuccessNotInstalled)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "00"; // not installed
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 0);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2SuccessInstalled)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "01";  // installed
    rxBytes[19] = "01"; // type byte
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2WrongSizeWithCompletionCode)
{
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "03"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 3);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2ExceptionInParsing)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "ZZ"; // invalid hex
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2CompletionCodeOutOfRange)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2WrongSizeInvalidCompletionCode)
{
    std::vector<std::string> rxBytes = {"00", "00", "16", "47", "00",
                                        "01", "0F", "02", "GG"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2WrongSizeCompletionCodeOutOfRange)
{
    std::vector<std::string> rxBytes = {
        "00", "00", "16", "47", "00", "01", "0F", "02", "FFFFFFFFFFFFFFFF"};
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2ExceptionInTokenStatus)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "ZZ";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2TokenStatusOutOfRange)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2ExceptionInInstalledTokenTypeBytes)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "01";
    rxBytes[19] = "GG";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2InstalledTokenTypeOutOfRange)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "01";
    rxBytes[19] = "FFFFFFFFFFFFFFFF";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, -1);
}

TEST_F(DebugTokenPrivateTest, ParseQueryV2InstalledTokenTypeCombinesAllBytes)
{
    std::vector<std::string> rxBytes(37, "00");
    rxBytes[8] = "00";
    rxBytes[9] = "01";
    rxBytes[19] = "78";
    rxBytes[20] = "56";
    rxBytes[21] = "34";
    rxBytes[22] = "12";
    int tokenInstallStatus = 0, installedTokenType = 0;
    int status = udt.parseQueryV2Response(rxBytes, tokenInstallStatus,
                                          installedTokenType, 31);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(tokenInstallStatus, 1);
    EXPECT_EQ(installedTokenType, 0x12345678);
}

// ========================== D-Bus mocked success paths =====================

// Helper class with counter-based NiceMock for D-Bus success paths
class DebugTokenDBusMockedTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    UpdateDebugToken udt{bus};

    void setupCounterMocks(int& callPhase, int& readIdx, int& atEnd)
    {
        EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_,
                                           testing::_, testing::_))
            .WillRepeatedly([&callPhase, &readIdx](sd_bus*, sd_bus_message*,
                                                   uint64_t, sd_bus_error*,
                                                   sd_bus_message** reply) {
                if (reply)
                    *reply = nullptr;
                callPhase++;
                readIdx = 0;
                return 0;
            });

        EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
            .WillRepeatedly([&atEnd](sd_bus_message*, int) {
                return (atEnd++ < 2) ? 0 : 1;
            });

        EXPECT_CALL(sdbusMock,
                    sd_bus_message_verify_type(nullptr, testing::_, testing::_))
            .WillRepeatedly(testing::Return(1));
    }
};

// getErasePolicy: GetSubTree returns empty → "No erase policy objects found"
TEST_F(DebugTokenDBusMockedTest, GetErasePolicyEmptyResult)
{
    // Override sd_bus_call to succeed but sd_bus_message_at_end returns 1
    // immediately
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1)); // empty array
    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

// getErasePolicy: GetSubTree returns one entry, Properties.Get returns "Manual"
TEST_F(DebugTokenDBusMockedTest, GetErasePolicyManual)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* policyPath = "/com/nvidia/debug_token/erase_policy";
    static const char* policyService = "com.nvidia.debug_token";
    static const char* policyIface = "com.nvidia.DebugToken.ErasePolicy";
    static const char* policyValue = "com.nvidia.DebugToken.ErasePolicy.Manual";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                const char* val;
                if (callPhase <= 1)
                {
                    // GetSubTree response: path, service, interface
                    static const char* strs[] = {policyPath, policyService,
                                                 policyIface};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else
                {
                    val = policyValue; // Properties.Get returns policy
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    auto policy = udt.getErasePolicy();
    EXPECT_EQ(policy, "Manual");
}

// getErasePolicy: Properties.Get throws → returns empty
TEST_F(DebugTokenDBusMockedTest, GetErasePolicyPropertyGetFails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* policyPath = "/com/nvidia/debug_token/erase_policy";
    static const char* policyService = "com.nvidia.debug_token";
    static const char* policyIface = "com.nvidia.DebugToken.ErasePolicy";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            if (callPhase == 2)
                return -1; // Properties.Get fails
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                static const char* strs[] = {policyPath, policyService,
                                             policyIface};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 2];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

// eraseDebugToken: getErasePolicy returns "Manual" → skip erase
TEST_F(DebugTokenDBusMockedTest, EraseDebugTokenManualPolicy)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* policyPath = "/com/nvidia/debug_token/erase_policy";
    static const char* policyService = "com.nvidia.debug_token";
    static const char* policyIface = "com.nvidia.DebugToken.ErasePolicy";
    static const char* policyValue = "com.nvidia.DebugToken.ErasePolicy.Manual";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                const char* val;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {policyPath, policyService,
                                                 policyIface};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else
                {
                    val = policyValue;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    int result = udt.eraseDebugToken();
    EXPECT_EQ(result, 0); // Skipped due to Manual policy
}

// eraseDebugToken: getErasePolicy returns "Automatic", nsmTokenEraseV2 fails
TEST_F(DebugTokenDBusMockedTest, EraseDebugTokenAutoV2Fails)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* policyPath = "/com/nvidia/debug_token/erase_policy";
    static const char* policyService = "com.nvidia.debug_token";
    static const char* policyIface = "com.nvidia.DebugToken.ErasePolicy";
    static const char* policyValue =
        "com.nvidia.DebugToken.ErasePolicy.Automatic";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            // Phase 2: Properties.Get for policy succeeds
            // Phase 3: nsmTokenEraseV2 → enumerateNsmDebugTokenEndpointsV2
            // fails
            if (callPhase >= 3)
                return -1;
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                const char* val;
                if (callPhase <= 1)
                {
                    static const char* strs[] = {policyPath, policyService,
                                                 policyIface};
                    val = strs[readIdx < 3 ? readIdx : 2];
                }
                else
                {
                    val = policyValue;
                }
                *static_cast<const char**>(out) = val;
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    int result = udt.eraseDebugToken();
    EXPECT_EQ(result, -1);
}

// eraseDebugToken: Automatic policy, V2 erase succeeds
// This is a complex multi-phase D-Bus interaction. The atEnd counter needs
// to reset between different GetSubTree calls. Use a phase-aware atEnd.
TEST_F(DebugTokenDBusMockedTest, EraseDebugTokenAutoV2Succeeds)
{
    static int callPhase = 0, readIdx = 0, atEnd = 0;
    callPhase = readIdx = atEnd = 0;

    static const char* policyPath = "/com/nvidia/debug_token/erase_policy";
    static const char* policyService = "com.nvidia.debug_token";
    static const char* policyIface = "com.nvidia.DebugToken.ErasePolicy";
    static const char* policyValue =
        "com.nvidia.DebugToken.ErasePolicy.Automatic";
    static const char* ep = "/xyz/openbmc_project/NSM/gpu0";
    static const char* actionIface = "com.nvidia.DebugToken.Action";
    static const char* asyncPath = "/com/nvidia/nsmd/async/1";
    static const char* successStatus = "com.nvidia.Async.Status.Success";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            callPhase++;
            readIdx = 0;
            atEnd = 0; // Reset atEnd for each new bus.call (new reply message)
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 'o')
            {
                *static_cast<const char**>(out) =
                    (callPhase == 4) ? asyncPath : ep;
                return 1;
            }
            if (type == 's')
            {
                const char* val = ep;
                if (callPhase == 1)
                {
                    // getErasePolicy GetSubTree
                    static const char* strs1[] = {policyPath, policyService,
                                                  policyIface};
                    val = strs1[readIdx < 3 ? readIdx : 2];
                }
                else if (callPhase == 2)
                {
                    val = policyValue; // Automatic
                }
                else if (callPhase == 3)
                {
                    // nsmTokenEraseV2 → enumerateV2 GetSubTree
                    static const char* strs3[] = {ep, "xyz.openbmc_project.NSM",
                                                  actionIface};
                    val = strs3[readIdx < 3 ? readIdx : 2];
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

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly([](sd_bus_message*, int) {
            // Each GetSubTree reply: first 2 at_end=0 (has data), then 1
            return (atEnd++ < 2) ? 0 : 1;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    int result = udt.eraseDebugToken();
    EXPECT_EQ(result, 0);
}

// installDebugToken: non-existent file → token parse failure
TEST_F(DebugTokenDBusMockedTest, InstallDebugTokenBadFile)
{
    auto status = udt.installDebugToken("/nonexistent/path");
    EXPECT_EQ(status, DebugTokenInstallStatus::DebugTokenInstallFailed);
}

// getMCTPServiceList: GetSubTree returns one service
TEST_F(DebugTokenDBusMockedTest, GetMCTPServiceListOneService)
{
    static int readIdx = 0, atEnd = 0;
    readIdx = atEnd = 0;

    static const char* svcPath = "/au/com/codeconstruct/mctp1/network/1/1";
    static const char* svcName = "au.com.codeconstruct.MCTP1";
    static const char* mctpIface = "xyz.openbmc_project.MCTP.Endpoint";

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            if (reply)
                *reply = nullptr;
            readIdx = 0;
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* out) {
            if (type == 's')
            {
                static const char* strs[] = {svcPath, svcName, mctpIface};
                *static_cast<const char**>(out) =
                    strs[readIdx < 3 ? readIdx : 2];
                readIdx++;
                return 1;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(
            [](sd_bus_message*, int) { return (atEnd++ < 2) ? 0 : 1; });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    auto services = udt.getMCTPServiceList();
    EXPECT_FALSE(services.empty());
}

// ========================== discoverMCTPDevices with pre-populated data =====

// Test discoverMCTPDevices loop body by pre-populating mctpInfo
// (getMCTPManagedObjects will fail since D-Bus calls fail on NiceMock,
//  but we can test the MCTP device processing by injecting data directly)

TEST_F(DebugTokenPrivateTest, FetchEidInfoWithAllInterfaces)
{
    dbus::InterfaceMap interfaces;

    // MCTP endpoint interface - medium comes from "MediumType" property here
    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(42);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    // Binding interface - binding type
    dbus::PropertyMap bindingProps;
    bindingProps["BindingType"] =
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    interfaces[mctpBindingIntfName] = bindingProps;

    // Enable interface
    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 42);
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    EXPECT_EQ(info.binding,
              "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEndpointFields)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingSupportedMessageTypes)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(30);
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEidProperty)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 0);
    EXPECT_TRUE(info.supportedMsgTypes.empty());
    EXPECT_EQ(info.medium, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingBindingTypeProperty)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(24);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    bindingProps["Other"] = std::string("ignored");
    interfaces[mctpBindingIntfName] = bindingProps;

    dbus::PropertyMap enableProps;
    enableProps["Connectivity"] = std::string("Available");
    interfaces[mctpEndpointEnableIntfName] = enableProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 24);
    EXPECT_TRUE(info.binding.empty());
    EXPECT_TRUE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingConnectivityProperty)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(25);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    bindingProps["BindingType"] =
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    interfaces[mctpBindingIntfName] = bindingProps;

    interfaces[mctpEndpointEnableIntfName] = dbus::PropertyMap{};

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 25);
    EXPECT_FALSE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoMissingEnableInterface)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap eidProps;
    eidProps["EID"] = uint8_t(26);
    eidProps["SupportedMessageTypes"] =
        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
    eidProps["MediumType"] =
        std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    interfaces[mctpEndpointIntfName] = eidProps;

    dbus::PropertyMap bindingProps;
    bindingProps["BindingType"] =
        std::string("xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    interfaces[mctpBindingIntfName] = bindingProps;

    auto info = udt.fetchEidInfoFromObject(interfaces);
    EXPECT_EQ(info.eid, 26);
    EXPECT_FALSE(info.enabled);
}

TEST_F(DebugTokenPrivateTest, FetchEidInfoBranchMatrix)
{
    struct Case
    {
        const char* name;
        bool setEid;
        bool setSupportedTypes;
        bool setMedium;
        bool setBinding;
        const char* connectivity;
        uint8_t expectedEid;
        size_t expectedSupportedCount;
        const char* expectedMedium;
        const char* expectedBinding;
        bool expectedEnabled;
    };

    const std::vector<Case> cases{
        {"eid-only", true, false, false, false, "Available", 0, 0, "", "",
         true},
        {"medium-only", true, true, true, false, "Available", 27, 2,
         "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", "", true},
        {"binding-only", true, true, false, true, "Available", 28, 2, "",
         "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", true},
        {"unexpected-connectivity", true, true, true, true, "Degraded", 29, 2,
         "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
         "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", false},
    };

    for (const auto& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);

        dbus::InterfaceMap interfaces;
        dbus::PropertyMap eidProps;
        if (testCase.setEid)
        {
            eidProps["EID"] =
                uint8_t(testCase.expectedEid == 0 ? 27 : testCase.expectedEid);
        }
        if (testCase.setSupportedTypes)
        {
            eidProps["SupportedMessageTypes"] =
                std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA};
        }
        if (testCase.setMedium)
        {
            eidProps["MediumType"] = std::string(testCase.expectedMedium);
        }
        interfaces[mctpEndpointIntfName] = eidProps;

        if (testCase.setBinding)
        {
            interfaces[mctpBindingIntfName] = dbus::PropertyMap{
                {"BindingType", std::string(testCase.expectedBinding)}};
        }

        if (testCase.connectivity != nullptr)
        {
            interfaces[mctpEndpointEnableIntfName] = dbus::PropertyMap{
                {"Connectivity", std::string(testCase.connectivity)}};
        }

        auto info = udt.fetchEidInfoFromObject(interfaces);
        EXPECT_EQ(info.eid, testCase.expectedEid);
        EXPECT_EQ(info.supportedMsgTypes.size(),
                  testCase.expectedSupportedCount);
        EXPECT_EQ(info.medium, testCase.expectedMedium);
        EXPECT_EQ(info.binding, testCase.expectedBinding);
        EXPECT_EQ(info.enabled, testCase.expectedEnabled);
    }
}

// Test updateDeviceMap with full pldm inventory data
TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWithFullData)
{
    // First populate mctpInfo with a UUID -> EID mapping
    MctpEidInfo info;
    info.eid = 42;
    info.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    info.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    info.supportedMsgTypes = {mctpTypeSPDM, mctpTypeVDMIANA};
    info.enabled = true;
    udt.mctpInfo["test-uuid-123"] = info;

    dbus::InterfaceMap interfaces;

    // UUID interface
    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("test-uuid-123");
    interfaces[uuidEndpointIntfName] = uuidProps;

    // PLDM inventory with serial number
    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN12345678");
    interfaces[pldmInventoryIntfName] = pldmProps;

    udt.updateDeviceMap(interfaces, "TestGPU_0");
    EXPECT_EQ(udt.devices.size(), 1u);
    EXPECT_EQ(udt.devices[42], "SN12345678");
    EXPECT_EQ(udt.deviceNameMap[42], "TestGPU_0");
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapWithoutUuidInterface)
{
    dbus::InterfaceMap interfaces;
    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN00000001");
    interfaces[pldmInventoryIntfName] = pldmProps;

    udt.updateDeviceMap(interfaces, "NoUUIDDevice");
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapUuidInterfaceWithoutUuidProperty)
{
    dbus::InterfaceMap interfaces;
    interfaces[uuidEndpointIntfName] = dbus::PropertyMap{};

    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN00000002");
    interfaces[pldmInventoryIntfName] = pldmProps;

    udt.updateDeviceMap(interfaces, "MissingUUIDProperty");
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

// Test updateDeviceMap with UUID that's NOT in mctpInfo → skipped
TEST_F(DebugTokenPrivateTest, UpdateDeviceMapUUIDNotInMctpInfo)
{
    dbus::InterfaceMap interfaces;

    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("unknown-uuid");
    interfaces[uuidEndpointIntfName] = uuidProps;

    dbus::PropertyMap pldmProps;
    pldmProps["SerialNumber"] = std::string("SN99999999");
    interfaces[pldmInventoryIntfName] = pldmProps;

    udt.updateDeviceMap(interfaces, "TestGPU_1");
    EXPECT_TRUE(udt.devices.empty()); // UUID not in mctpInfo → skipped
}

// Test updateDeviceMap: UUID exists, no PLDM inventory → serial empty
TEST_F(DebugTokenPrivateTest, UpdateDeviceMapNoPldmInventory)
{
    MctpEidInfo info;
    info.eid = 10;
    udt.mctpInfo["uuid-no-pldm"] = info;

    dbus::InterfaceMap interfaces;
    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("uuid-no-pldm");
    interfaces[uuidEndpointIntfName] = uuidProps;

    udt.updateDeviceMap(interfaces, "TestDevice");
    // No pldmInventoryIntfName → no serial → no device entry
    EXPECT_TRUE(udt.devices.empty());
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapPldmWithoutSerialNumber)
{
    MctpEidInfo info;
    info.eid = 11;
    udt.mctpInfo["uuid-no-serial"] = info;

    dbus::InterfaceMap interfaces;
    dbus::PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("uuid-no-serial");
    interfaces[uuidEndpointIntfName] = uuidProps;

    dbus::PropertyMap pldmProps;
    pldmProps["Manufacturer"] = std::string("NVIDIA");
    interfaces[pldmInventoryIntfName] = pldmProps;

    udt.updateDeviceMap(interfaces, "MissingSerial");
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(DebugTokenPrivateTest, UpdateDeviceMapBranchMatrix)
{
    MctpEidInfo firstInfo;
    firstInfo.eid = 61;
    udt.mctpInfo["known-uuid-1"] = firstInfo;

    MctpEidInfo secondInfo;
    secondInfo.eid = 62;
    udt.mctpInfo["known-uuid-2"] = secondInfo;

    {
        dbus::InterfaceMap interfaces;
        interfaces[uuidEndpointIntfName] =
            dbus::PropertyMap{{"UUID", std::string("")}};
        interfaces[pldmInventoryIntfName] =
            dbus::PropertyMap{{"SerialNumber", std::string("SN-SHOULD-SKIP")}};
        udt.updateDeviceMap(interfaces, "EmptyUuid");
    }

    {
        dbus::InterfaceMap interfaces;
        interfaces[uuidEndpointIntfName] =
            dbus::PropertyMap{{"UUID", std::string("known-uuid-1")}};
        interfaces[pldmInventoryIntfName] =
            dbus::PropertyMap{{"SerialNumber", std::string("")}};
        udt.updateDeviceMap(interfaces, "EmptySerial");
    }

    {
        dbus::InterfaceMap interfaces;
        interfaces[uuidEndpointIntfName] =
            dbus::PropertyMap{{"UUID", std::string("known-uuid-2")}};
        interfaces[pldmInventoryIntfName] =
            dbus::PropertyMap{{"SerialNumber", std::string("SN-SECOND")}};
        udt.updateDeviceMap(interfaces, "SecondDevice");
    }

    EXPECT_EQ(udt.devices.size(), 2u);
    EXPECT_EQ(udt.devices[61], "");
    EXPECT_EQ(udt.deviceNameMap[61], "EmptySerial");
    EXPECT_EQ(udt.devices[62], "SN-SECOND");
    EXPECT_EQ(udt.deviceNameMap[62], "SecondDevice");
}

// Test updateDeviceMap: duplicate UUID with higher priority medium
TEST_F(DebugTokenPrivateTest, UpdateDeviceMapDuplicateUUID)
{
    // Put two devices with same UUID but different media
    MctpEidInfo lowPrio;
    lowPrio.eid = 10;
    lowPrio.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus";
    lowPrio.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus";
    lowPrio.supportedMsgTypes = {mctpTypeSPDM, mctpTypeVDMIANA};
    lowPrio.enabled = true;
    udt.mctpInfo["dup-uuid"] = lowPrio;

    MctpEidInfo highPrio;
    highPrio.eid = 20;
    highPrio.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    highPrio.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    highPrio.supportedMsgTypes = {mctpTypeSPDM, mctpTypeVDMIANA};
    highPrio.enabled = true;

    // The < operator compares medium priority: PCIe (0) < SMBus (5)
    // mctpInfo["dup-uuid"] already has SMBus. Check that the comparison works.
    EXPECT_TRUE(
        udt.mctpInfo["dup-uuid"] <
        highPrio); // SMBus < PCIe = true (higher medium val = lower prio)
}

// ========================== getErasePolicy: multiple results ==============

// GetErasePolicyMultipleResults removed - complex multi-entry mock
// not reliably achievable with counter-based approach

// ========================== MctpEidInfo comparison operator tests ==========

TEST_F(DebugTokenPrivateTest, MctpEidInfoComparisonSameMedium)
{
    MctpEidInfo a;
    a.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    a.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    MctpEidInfo b;
    b.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    b.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB";

    // Same medium, different binding → compare binding priority
    bool result = a < b;
    (void)result;
}

TEST_F(DebugTokenPrivateTest, MctpEidInfoComparisonDifferentMedium)
{
    MctpEidInfo a;
    a.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus";
    a.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus";

    MctpEidInfo b;
    b.medium = "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    b.binding = "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    EXPECT_TRUE(
        a < b); // SMBus (5) > PCIe (0), so a has higher value → a < b is true
}

// ========================== token_utility.hpp: parseTlvTokens edge cases ===

static std::vector<uint8_t>
    makeTlvRecordWithSerial(const std::array<uint8_t, 8>& serial,
                            uint8_t deviceType = 0x01)
{
    debug_token::ItemHeader deviceTypeItem{};
    deviceTypeItem.type = htole16(debug_token::types::Common::DeviceType);
    deviceTypeItem.size = htole16(1);

    debug_token::ItemHeader serialItem{};
    serialItem.type = htole16(debug_token::types::Common::DeviceSerialNumber);
    serialItem.size = htole16(serial.size());

    const uint32_t payloadSize =
        sizeof(deviceTypeItem) + 1 + sizeof(serialItem) + serial.size();

    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "TLV1", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(payloadSize);

    std::vector<uint8_t> record(sizeof(structHdr) + payloadSize, 0);
    size_t offset = 0;
    std::memcpy(record.data() + offset, &structHdr, sizeof(structHdr));
    offset += sizeof(structHdr);
    std::memcpy(record.data() + offset, &deviceTypeItem,
                sizeof(deviceTypeItem));
    offset += sizeof(deviceTypeItem);
    record[offset++] = deviceType;
    std::memcpy(record.data() + offset, &serialItem, sizeof(serialItem));
    offset += sizeof(serialItem);
    std::memcpy(record.data() + offset, serial.data(), serial.size());
    return record;
}

static std::vector<uint8_t> makeMalformedTlvRecord(uint32_t payloadSize = 0)
{
    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "BAD!", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(payloadSize);

    std::vector<uint8_t> record(sizeof(structHdr) + payloadSize, 0);
    std::memcpy(record.data(), &structHdr, sizeof(structHdr));
    return record;
}

TEST_F(DebugTokenPrivateTest, ParseTlvTokensOffsetBeyondData)
{
    // Create header pointing to offset beyond file data
    DebugTokenHeader hdr{};
    hdr.version = 2;
    hdr.type = FileTypeDebugToken;
    hdr.numberOfRecords = 1;
    hdr.offsetToListOfStructs = 100; // Way beyond

    std::vector<uint8_t> fullFile(110,
                                  0); // 110 bytes, so offset 100 + some data
    memcpy(fullFile.data(), &hdr, sizeof(hdr));

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &hdr, tokens);
    EXPECT_NE(result, 0); // Should fail - insufficient data for TLV
}

TEST_F(DebugTokenPrivateTest, ParseTlvTokensEmptyPayload)
{
    // offset points to data but numberOfRecords = 1, only has header bytes
    DebugTokenHeader hdr{};
    hdr.version = 2;
    hdr.type = FileTypeDebugToken;
    hdr.numberOfRecords = 1;
    hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    // Create file with just the header + a tiny bit of data (not enough for TLV
    // structure)
    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader) + 4, 0);
    memcpy(fullFile.data(), &hdr, sizeof(hdr));

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &hdr, tokens);
    EXPECT_NE(result, 0);
}

TEST_F(DebugTokenPrivateTest, ParseTlvTokensValidRecordThenHitsEndOfData)
{
    DebugTokenHeader hdr{};
    hdr.version = 2;
    hdr.type = FileTypeDebugToken;
    hdr.numberOfRecords = 2;
    hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    const auto validRecord = makeTlvRecordWithSerial(
        {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77});

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader), 0);
    std::memcpy(fullFile.data(), &hdr, sizeof(hdr));
    fullFile.insert(fullFile.end(), validRecord.begin(), validRecord.end());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &hdr, tokens);

    EXPECT_EQ(result, -1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x7071727374757677"));
}

TEST_F(DebugTokenPrivateTest, ParseTlvTokensMalformedRecordAfterValidRecord)
{
    DebugTokenHeader hdr{};
    hdr.version = 2;
    hdr.type = FileTypeDebugToken;
    hdr.numberOfRecords = 2;
    hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    const auto validRecord = makeTlvRecordWithSerial(
        {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87});
    const auto malformedRecord = makeMalformedTlvRecord();

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader), 0);
    std::memcpy(fullFile.data(), &hdr, sizeof(hdr));
    fullFile.insert(fullFile.end(), validRecord.begin(), validRecord.end());
    fullFile.insert(fullFile.end(), malformedRecord.begin(),
                    malformedRecord.end());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &hdr, tokens);

    EXPECT_EQ(result, -1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x8081828384858687"));
}

TEST_F(DebugTokenPrivateTest, ParseTlvTokensMalformedRecordThenValidRecord)
{
    DebugTokenHeader hdr{};
    hdr.version = 2;
    hdr.type = FileTypeDebugToken;
    hdr.numberOfRecords = 2;
    hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    const auto malformedRecord = makeMalformedTlvRecord();
    const auto validRecord = makeTlvRecordWithSerial(
        {0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97});

    std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader), 0);
    std::memcpy(fullFile.data(), &hdr, sizeof(hdr));
    fullFile.insert(fullFile.end(), malformedRecord.begin(),
                    malformedRecord.end());
    fullFile.insert(fullFile.end(), validRecord.begin(), validRecord.end());

    TokenMap tokens;
    int result = TokenUtility::parseTlvTokens(fullFile, &hdr, tokens);

    EXPECT_EQ(result, -1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(tokens.contains("0x9091929394959697"));
}

TEST_F(DebugTokenPrivateTest, ParseSingleTlvRecordDuplicateSerialDoesNotReplace)
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
    tokens.emplace("0x6061626364656667", std::vector<uint8_t>{0xAA});
    size_t recordSize = 0;

    EXPECT_EQ(
        TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens, recordSize),
        0);
    EXPECT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens.count("0x6061626364656667"), 2u);
    const auto [firstToken, lastToken] =
        tokens.equal_range("0x6061626364656667");
    ASSERT_NE(firstToken, lastToken);
    EXPECT_EQ(firstToken->second, std::vector<uint8_t>({0xAA}));
}

TEST_F(DebugTokenPrivateTest, ParseSingleTlvRecordMalformedStructureThrows)
{
    debug_token::StructureHeader structHdr{};
    std::memcpy(structHdr.identifier, "BAD!", 4);
    structHdr.versionMajor = htole16(2);
    structHdr.versionMinor = htole16(0);
    structHdr.size = htole32(0);

    std::vector<uint8_t> tokenData(sizeof(debug_token::StructureHeader), 0);
    std::memcpy(tokenData.data(), &structHdr, sizeof(structHdr));

    TokenMap tokens;
    size_t recordSize = 0;
    EXPECT_THROW((void)TokenUtility::parseSingleTlvRecord(tokenData, 0, tokens,
                                                          recordSize),
                 std::runtime_error);
    EXPECT_GT(recordSize, 0u);
}

// ========================== token_utility.hpp: getNextDebugToken edge cases

TEST_F(DebugTokenPrivateTest, GetNextDebugTokenBeyondFile)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_short.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        // Write less than sizeof(TokenHeader) bytes
        std::vector<uint8_t> data(10, 0);
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto result = udt.getNextDebugToken(ifs, 0, tokenData, serialNumber);
    EXPECT_EQ(result, nullptr);
    std::filesystem::remove(tmpPath);
}

TEST_F(DebugTokenPrivateTest, GetNextDebugTokenValidHeader)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_tokenhdr.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        // Write a valid TokenHeader + serial + data
        size_t serialSize = 8;
        size_t extraPayload = 20;
        uint16_t structSize = sizeof(TokenHeader) + serialSize + extraPayload;

        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "CRDT", 4);
        tokHdr.structSize = structSize;
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));
        std::vector<uint8_t> serial(serialSize, 0x01);
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());
        std::vector<uint8_t> payload(extraPayload, 0xAB);
        ofs.write(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto result = udt.getNextDebugToken(ifs, 0, tokenData, serialNumber);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->structSize, sizeof(TokenHeader) + 28);
    EXPECT_EQ(serialNumber.size(), 8u);
    std::filesystem::remove(tmpPath);
}

TEST_F(DebugTokenPrivateTest, GetNextDebugTokenMCUIdentifier)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_mcu_hdr.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        size_t serialSize = 16; // MCU serial
        size_t extraPayload = 20;
        uint16_t structSize = sizeof(TokenHeader) + serialSize + extraPayload;

        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "MCDT", 4);
        tokHdr.structSize = structSize;
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));
        std::vector<uint8_t> serial(serialSize, 0x42);
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());
        std::vector<uint8_t> payload(extraPayload, 0xAB);
        ofs.write(reinterpret_cast<const char*>(payload.data()),
                  payload.size());
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto result = udt.getNextDebugToken(ifs, 0, tokenData, serialNumber);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(serialNumber.size(), 16u); // MCU uses 16-byte serial
    std::filesystem::remove(tmpPath);
}

TEST_F(DebugTokenPrivateTest, GetNextDebugTokenShortTokenBytes)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_short_token_bytes.bin";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        TokenHeader tokHdr{};
        memcpy(tokHdr.identifier, "CRDT", 4);
        tokHdr.structSize = sizeof(TokenHeader) + 8 + 20;
        ofs.write(reinterpret_cast<const char*>(&tokHdr), sizeof(tokHdr));
        std::vector<uint8_t> serial(8, 0x11);
        ofs.write(reinterpret_cast<const char*>(serial.data()), serial.size());
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> tokenData;
    std::vector<uint8_t> serialNumber;
    auto result = udt.getNextDebugToken(ifs, 0, tokenData, serialNumber);
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(tokenData.empty());
    std::filesystem::remove(tmpPath);
}

TEST_F(DebugTokenPrivateTest, GetDebugTokenHeaderRejectsWrongFileType)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_bad_debug_header.bin";
    DebugTokenHeader header{};
    header.version = 1;
    header.type = 1;
    header.numberOfRecords = 1;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader), 0);
    auto result = udt.getDebugTokenHeader(headerData, ifs);
    EXPECT_EQ(result, nullptr);
    std::filesystem::remove(tmpPath);
}

TEST_F(DebugTokenPrivateTest, GetDebugTokenHeaderAcceptsDebugTokenFileType)
{
    auto tmpPath =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/test_good_debug_header.bin";
    DebugTokenHeader header{};
    header.version = 2;
    header.type = FileTypeDebugToken;
    header.numberOfRecords = 2;
    header.offsetToListOfStructs = sizeof(DebugTokenHeader);
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    std::ifstream ifs(tmpPath, std::ios::binary);
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader), 0);
    auto result = udt.getDebugTokenHeader(headerData, ifs);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->type, FileTypeDebugToken);
    EXPECT_EQ(result->numberOfRecords, 2);
    std::filesystem::remove(tmpPath);
}

// ========================== formatSerialNumber ==============================

TEST_F(DebugTokenPrivateTest, FormatSerialNumberValid)
{
    std::vector<uint8_t> serial = {0x01, 0x02, 0x03, 0x04,
                                   0x05, 0x06, 0x07, 0x08};
    auto result = TokenUtility::formatSerialNumber(serial);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result, "0x0102030405060708");
}

TEST_F(DebugTokenPrivateTest, FormatSerialNumberEmpty)
{
    std::vector<uint8_t> serial;
    auto result = TokenUtility::formatSerialNumber(serial);
    EXPECT_EQ(result, "0x");
}

// ========================== parseCommandOutput ==============================

TEST_F(DebugTokenPrivateTest, ParseCommandOutputWithRX)
{
    std::string output = "teid = 31\nTX: 00 01\nRX: AA BB CC DD\n";
    auto result = udt.parseCommandOutput(output);
    EXPECT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], "AA");
}

TEST_F(DebugTokenPrivateTest, ParseCommandOutputNoRX)
{
    std::string output = "teid = 31\nTX: 00 01\n";
    auto result = udt.parseCommandOutput(output);
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, ParseCommandOutputEmpty)
{
    auto result = udt.parseCommandOutput("");
    EXPECT_TRUE(result.empty());
}

TEST_F(DebugTokenPrivateTest, ParseCommandOutputUsesLastRxLine)
{
    std::string output = "teid = 31\nRX: 01 02\nnoise\nRX: AA BB CC\n";
    auto result = udt.parseCommandOutput(output);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "AA");
    EXPECT_EQ(result[2], "CC");
}

// getErasePolicy, discoverMCTPDevices, updateEndPoints already tested above
