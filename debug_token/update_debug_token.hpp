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

#pragma once
#include "config.h"

#include "tlv/tlv.h"

#include "token_utility.hpp"

#include <fmt/format.h>

#include <fstream>
#include <map>
#include <mutex>
#include <tuple>

namespace dbus
{

using ObjectPath = std::string;
using Interface = std::string;
using Interfaces = std::vector<std::string>;
using Property = std::string;
using PropertyType = std::string;
using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;

using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
using Service = std::string;
using Interfaces = std::vector<Interface>;
using MapperServiceMap = std::vector<std::pair<Service, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;
} // namespace dbus

using UUID = std::string;
using EID = uint8_t;
using SupportedMessageTypes = std::vector<uint8_t>;
using DeviceName = std::string;
using DeviceMap = std::map<EID, SerialNumber>;
using DeviceNameMap = std::map<EID, DeviceName>;
using NSMStatusMap = std::map<DeviceName, int>;
using MctpMedium = std::string;
using MctpBinding = std::string;
using Message = std::string;
using Resolution = std::string;
using MessageMapping = std::pair<Message, Resolution>;
using NSMEndpoints = std::vector<std::string>;
using NSMTokenStatusTuple =
    std::tuple<std::string, std::string, std::string, uint32_t>;
using NSMErrorTuple = std::tuple<uint16_t, std::string>;
using NSMAsyncValue = std::variant<NSMTokenStatusTuple, NSMErrorTuple>;

using namespace dbus;
namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

static constexpr uint8_t mctpTypeSPDM = 0x5;
static constexpr uint8_t mctpTypeVDMIANA = 0x7f;
constexpr auto erasePolicyIntfName = "com.nvidia.DebugToken.ErasePolicy";
constexpr auto erasePolicyPath = "/com/nvidia/debug_token/";
constexpr auto mctpPCIeService = "xyz.openbmc_project.MCTP.Control.PCIe";
constexpr auto mctpPath = "/au/com/codeconstruct/mctp1";
constexpr auto objectMapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperIntfName = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mctpEndpointIntfName = "xyz.openbmc_project.MCTP.Endpoint";
constexpr auto objectEnableIntfName = "xyz.openbmc_project.Object.Enable";
constexpr auto mctpEndpointEnableIntfName =
    "au.com.codeconstruct.MCTP.Endpoint1";
constexpr auto uuidEndpointIntfName = "xyz.openbmc_project.Common.UUID";
constexpr auto mctpBindingIntfName = "xyz.openbmc_project.MCTP.Binding";
constexpr auto pldmService = "xyz.openbmc_project.PLDM";
constexpr auto pldmPath = "/";
constexpr auto pldmInventoryIntfName =
    "xyz.openbmc_project.Inventory.Decorator.Asset";

constexpr auto nsmService = "xyz.openbmc_project.NSM";
constexpr auto nsmDebugTokenIntfName = "com.nvidia.DebugToken";
constexpr auto nsmDebugTokenActionIntfName = "com.nvidia.DebugToken.Action";
constexpr auto nsmDebugTokenStatusIntfName = "com.nvidia.DebugToken.Status";
constexpr auto nsmAsyncStatusIntfName = "com.nvidia.Async.Status";
constexpr auto nsmAsyncValueIntfName = "com.nvidia.Async.Value";
constexpr auto nsmAsyncBasePath = "/com/nvidia/nsmd/AsyncOperation";
constexpr auto nsmDebugTokenPath = "/";
constexpr auto propertiesIntfName = "org.freedesktop.DBus.Properties";

constexpr auto nsmTokenTypeCRDT = "com.nvidia.DebugToken.TokenTypes.CRDT";
constexpr uint32_t EraseAll = 0xFFFFFFFF;
constexpr auto nsmTokenStatusDebugSessionActive =
    "com.nvidia.DebugToken.TokenStatus.DebugSessionActive";
constexpr auto nsmTokenStatusTokenTimeout =
    "com.nvidia.DebugToken.TokenStatus.TokenTimeout";
constexpr auto nsmTokenStatusNoTokenApplied =
    "com.nvidia.DebugToken.TokenStatus.NoTokenApplied";

const std::string mctpVdmUtilPath = "/usr/bin/mctp-vdm-util";
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};
const std::string debugTokenEraseFailed{
    "NvidiaUpdate.1.0.DebugTokenEraseFailed"};
static constexpr size_t mctpCompletionCodeByte =
    8; // 8'th byte from beginning is the MCTP Completion code for debug token
       // query
static constexpr size_t tokenInstallStatusByteV1 =
    9; // 9th byte from beginning is token status code for debug token query
static constexpr size_t tokenInstallStatusByteV2 =
    9; // 9th byte from beginning is token status code for debug token query
static constexpr size_t tokenInstallStatusByteV3 =
    12; // 12th byte from beginning is token status code for debug token query
static constexpr size_t mctpDebugTokenQueryResponseLengthV1 =
    19; // Total length of MCTP response : Header (9) + Data (10)
static constexpr size_t mctpDebugTokenQueryResponseLengthV2 =
    37; // Total length of MCTP response : Header (9) + Data (28)
static constexpr size_t mctpDebugTokenQueryResponseLengthV3 =
    50; // Total length of MCTP response : Header (9) + Data (41)
static constexpr uint64_t propertyChangeSignalTimeout = 5;

// Token type bytes in v2 query command are bytes 19-22
static constexpr int tokenTypeByteStartV2 = 19;
static constexpr int tokenTypeByteEndV2 = 22;
// Token type bytes in v3 query command are bytes 30-33
static constexpr int tokenTypeByteStartV3 = 30;
static constexpr int tokenTypeByteEndV3 = 33;
static constexpr int nsmUnsupportedCmd = 0x05;

static constexpr size_t debugFirmwareTokenType = 0x1;

using Priority = int;

static std::unordered_map<MctpMedium, Priority> mediumPriority = {
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 5},
};

static std::unordered_map<MctpBinding, Priority> bindingPriority = {
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", 5},
};

struct MctpEidInfo
{
    EID eid;
    MctpMedium medium;
    MctpBinding binding;
    SupportedMessageTypes supportedMsgTypes;
    bool enabled;

    friend bool operator<(MctpEidInfo const& lhs, MctpEidInfo const& rhs)
    {

        if (mediumPriority.at(lhs.medium) == mediumPriority.at(rhs.medium))
            return bindingPriority.at(lhs.binding) >
                   bindingPriority.at(rhs.binding);
        else
            return mediumPriority.at(lhs.medium) >
                   mediumPriority.at(rhs.medium);
    }
};
using MctpInfo = std::map<UUID, MctpEidInfo>;

enum class OperationType
{
    TokenInstall,
    TokenErase,
    BackgroundCopy,
    TokenQueryStatus,
    Common
};

/* Debug token install command error codes */
enum class InstallErrorCodes
{
    InstallSuccess = 0x0,
    InvalidToken,
    TokenAuthFailed,
    TokenNonceInvalid,
    TokenSerialNumberInvalid,
    TokenECFWVersionInvalid,
    DisableBackgroundCopyCheckFailed,
    InstallInternalError,
    NsmInstallError
};

enum class NSMTokenStatus
{
    Error = 0x0,
    DebugSessionActive = 0x2,
    NoTokenApplied = 0x3,
    ChallengeProvided = 0x4,
    TokenInstallTimeout = 0x5,
    TokenTimeout = 0x6
};

enum class MCTPCompletionCodes
{
    Success = 0x0,
    Error,
    ErrorInvalidData,
    ErrorInvalidLength,
    ErrorNotReady,
    ErrorUnsupportedCmd
};

/* Debug token install error code mapping for message registry */
static std::map<InstallErrorCodes, MessageMapping> installErrorMapping{
    {InstallErrorCodes::InvalidToken,
     {"Invalid Debug Token for {}.",
      "Check Debug Token is valid and signed by NVIDIA. Request the"
      " debug token again and retry with new debug signed firmware package."}},
    {InstallErrorCodes::TokenAuthFailed,
     {"Debug Token Authentication failed for {}",
      "Check Debug Token is valid and signed by NVIDIA. Request the"
      " debug token again and retry with new debug signed firmware package."}},
    {InstallErrorCodes::TokenNonceInvalid,
     {"Debug Token Nonce invalid for {}",
      "Request the debug token again and retry with new debug signed"
      " firmware package."}},
    {InstallErrorCodes::TokenSerialNumberInvalid,
     {"Debug Token Serial Number invalid for {}",
      "Check debug token was generated for this system and retry the"
      " firmware update operation with valid debug signed firmware package."}},
    {InstallErrorCodes::TokenECFWVersionInvalid,
     {"Debug Token ERoT Firmware Version invalid for {}",
      "Request the debug token with valid ERoT firmware version and retry with"
      " firmware update operation with new debug signed firmware package."}},
    {InstallErrorCodes::DisableBackgroundCopyCheckFailed,
     {"Disabling BackgroundCopy Check Failed for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {InstallErrorCodes::InstallInternalError,
     {"Debug Token Install Internal Error for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
};

/* Debug token erase command error codes */
enum class EraseErrorCodes
{
    EraseSuccess = 0x0,
    EraseInternalError = 0x1,
    EraseFailed = 0x2,
};

/* Debug token erase error code mapping for message registry */
static std::map<EraseErrorCodes, MessageMapping> eraseErrorMapping{
    {EraseErrorCodes::EraseInternalError,
     {"Debug Token Erase Internal Error for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {EraseErrorCodes::EraseFailed,
     {"Erase failed for one or more devices.",
      "No action required. If there are other component failures in task, retry"
      " the firmware update operation and if issue still persists reset the "
      "baseboard."}}};

/* background copy enabled or disable command error codes */
enum class BackgroundCopyErrorCodes
{
    BackgroundCopySuccess = 0x0,
    BackgroundCopyFailed = 0x1,
    BackgroundEnableFail = 0x2,
    BackgroundDisableFail = 0x3
};

/* Background copy error code mapping for message registry */
static std::map<BackgroundCopyErrorCodes, MessageMapping>
    backgroundCopyErrorMapping{
        {BackgroundCopyErrorCodes::BackgroundEnableFail,
         {"Enabling Background Copy Failed for {}",
          "Retry the firmware update operation and if issue still persists reset"
          " the baseboard."}},
        {BackgroundCopyErrorCodes::BackgroundDisableFail,
         {"Disabling Background Copy Failed for {}",
          "Retry the firmware update operation and if issue still persists reset"
          " the baseboard."}}};

/* Debug token query error codes */
enum class DebugTokenQueryErrorCodes
{
    DebugTokenNotInstalled = 0x0,
    DebugTokenInstalled = 0x1
};

/* Debug token query error codes */
enum class CommonErrorCodes
{
    MCTPDiscoveryFailed = 0x1,
    TokenParseFailure,
    MCTPCommandInstallSuccess,
    MCTPCommandInstallFailure,
    MCTPCommandEraseSuccess,
    MCTPCommandEraseFailure,
    MCTPResponseInstallFailure,
    MCTPResponseEraseFailure,
    NSMCommandInstallSuccess,
    NSMCommandInstallFailure,
    NSMCommandEraseSuccess,
    NSMCommandEraseFailure,
    NSMCommandFailure,
};

/* debug token common error code mapping for message registry */
static const std::map<CommonErrorCodes, MessageMapping> debugTokenCommonErrorMapping{
    {CommonErrorCodes::MCTPDiscoveryFailed,
     {"Device Discovery Failure",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::TokenParseFailure,
     {"Invalid Debug Token File",
      "Check FW Package contains valid debug token file and retry"
      " the operation."}},
    {CommonErrorCodes::MCTPCommandInstallFailure,
     {"Transferring Debug Token to ERoT failed for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::MCTPCommandInstallSuccess,
     {"Debug token installed on {}.", ""}},
    {CommonErrorCodes::MCTPCommandEraseFailure,
     {"Request to Erase Debug Token failed for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::MCTPCommandEraseSuccess,
     {"Debug Token erased on {}.", ""}},
    {CommonErrorCodes::MCTPResponseInstallFailure,
     {"Debug Token Install response is invalid for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::MCTPResponseEraseFailure,
     {"Debug Token Erase response is invalid for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::NSMCommandInstallFailure,
     {"Debug Token Install failure for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::NSMCommandInstallSuccess,
     {"Debug Token installed on {}", ""}},
    {CommonErrorCodes::NSMCommandEraseFailure,
     {"Debug Token Erase failure for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::NSMCommandEraseSuccess,
     {"Debug Token erased on {}", ""}},
    {CommonErrorCodes::NSMCommandFailure,
     {"NSM Command failure for {}",
      "No action required. If there are other component failures in task, retry"
      " the firmware update operation and if issue still persists reset the "
      "baseboard."}},
};

/* Debug Token Install Status Codes*/
enum class DebugTokenInstallStatus
{
    DebugTokenInstallSuccess = 0,
    DebugTokenInstallFailed = 1,
    DebugTokenInstallNone = 2
};

/**
 * @brief implemementation of update debug token utility
 *
 */
class UpdateDebugToken : public TokenUtility
{
  public:
    /**
     * @brief Construct a new Debug token ItemUpdater object
     *
     * @param[in] bus
     */
    UpdateDebugToken(sdbusplus::bus::bus& bus) : bus(bus)
    {}
    /**
     * @brief install debug token for all matching devices
     *
     * @param[in] debugTokenPath
     *
     * @return DebugTokenInstallStatus
     */
    DebugTokenInstallStatus
        installDebugToken(const std::string& debugTokenPath);
    /**
     * @brief erase debug token for all discovered devices
     *
     * @return int
     */
    int eraseDebugToken();
    /**
     * @brief Parse and update token map with serial number to token data
     * mapping. Supports both TLV v2.0 and legacy token formats.
     *
     * @param[in] debugTokenPath - debug token file path
     * @param[out] tokens - token map (serial number -> token data)
     *
     * @return int status code (0 on success, -1 on failure)
     */
    int updateTokenMap(const std::string& debugTokenPath, TokenMap& tokens);
    /**
     * @brief log message registry entry
     *
     * @param[in] messageID - redfish message
     * @param[in] compName - component name
     * @param[in] compVersion - component version
     *
     * @return void
     */
    void createMessageRegistry(const std::string& messageID,
                               const std::string& compName,
                               const std::string& compVersion);

    /**
     * @brief Get the Message for debug token enhanced message registry
     *
     * @param[in] operationType - debug token operation type
     * @param[in] errorCode - error code
     * @param[in] deviceName - optional device name
     * @return error and resolution - if error code mapping is present
     */
    std::optional<std::tuple<std::string, std::string>>
        getMessage(const OperationType operationType, const int errorCode,
                   const std::string deviceName = {});

    /**
     * @brief Create a Message Registry for Resource Event Errors
     *
     * @param[in] messageID - redfish message id
     * @param[in] ComponentName - redfish
     * @param[in] operationType - debug token operation type
     * @param[in] errorCode - debug token error code
     * @param[in] deviceName - device name
     */
    void createMessageRegistryResourceErrors(const std::string& messageID,
                                             const std::string& componentName,
                                             const OperationType& operationType,
                                             const int& errorCode,
                                             const std::string deviceName = {});
    /**
     * @brief format error message with device name
     *
     * @param[in] message
     * @param[in] deviceName
     * @return std::string - formatted error message
     */
    std::string formatMessage(const std::string& message,
                              const std::string& deviceName)
    {
        if (deviceName.empty())
        {
            return message;
        }
        else
        {
            return fmt::format(fmt::runtime(message), deviceName);
        }
    }

    /**
     * @brief Create a Message Registry for Install Errors
     *
     * @param[in] path
     */
    void createTokenInstallErrorMessage(std::string path)
    {
        this->createMessageRegistryResourceErrors(
            transferFailed, DEBUG_TOKEN_INSTALL_NAME, OperationType::Common,
            static_cast<int>(CommonErrorCodes::NSMCommandInstallFailure), path);
    }

    /**
     * @brief Create a Message Registry for Erase Errors
     *
     * @param[in] path
     */
    void createTokenEraseErrorMessage(std::string path)
    {
        this->createMessageRegistryResourceErrors(
            transferFailed, DEBUG_TOKEN_ERASE_NAME, OperationType::Common,
            static_cast<int>(CommonErrorCodes::NSMCommandEraseFailure), path);
    }
    /**
     * @brief Parse query v2 response.
     *
     * @param[in] rxBytes
     * @param[in] tokenInstallStatus
     * @param[in] installedTokenType
     * @param[in] eid
     *
     * @return int
     */
    int parseQueryV2Response(std::vector<std::string> rxBytes,
                             int& tokenInstallStatus, int& installedTokenType,
                             const EID& eid);

  private:
    sdbusplus::bus::bus& bus;
    /* device map of EID to serial number */
    DeviceMap devices;
    /* map of UUID to EID */
    MctpInfo mctpInfo;
    /* Mutex for NSM async status handling. */
    std::mutex mtx;

    /* component name map for message registry */
    DeviceNameMap deviceNameMap;
    /**
     * @brief Parses Eid info given an interface map
     *
     * @param interfaces - interface map
     *
     * @return MctpEidInfo - Eid info
     */
    MctpEidInfo fetchEidInfoFromObject(const dbus::InterfaceMap& interfaces);
    /**
     * @brief retrieve MCTP managed objects
     *
     * @return dbus::OjectValueTree - map of objects to values
     */
    dbus::ObjectValueTree getMCTPManagedObjects();

    /**
     * @brief Method to check supported message types to perform debug token
     * erase/install operation
     *
     * @param[in] supportedMsgTypes - mctp supported message types
     * @param[in] eid - eid for logging
     * @return true
     * @return false
     */
    bool checkSupportForSPDMandMCTPVDM(
        const SupportedMessageTypes& supportedMsgTypes, EID eid);
    /**
     * @brief discover MCTP end points
     *
     * @return int - status code
     */
    int discoverMCTPDevices();
    /**
     * @brief Get debug token erase policy from DBus
     *
     * @return std::string - erase policy (automatic or manual)
     */
    std::string getErasePolicy();
    /**
     * @brief Retrieve Services that contain objects with
     *        MCTP Endpoint Interface
     *
     * @return std::set<Service> - Set of services
     */
    std::set<dbus::Service> getMCTPServiceList();
    /**
     * @brief update device map of eid->serial number
     *
     * @param[in] interfaces
     * @param[in] deviceName
     */
    void updateDeviceMap(const dbus::InterfaceMap& interfaces,
                         const std::string& deviceName);
    /**
     * @brief update end points, discover MCTP end points, get serial number
     * from pldm D-Bus object and create a map of serial number to EID
     *
     * @return int
     */
    int updateEndPoints();
    /**
     * @brief enable background copy
     *
     * @param[in] eid
     *
     * @return int
     */
    int enableBackgroundCopy(const EID& eid);
    /**
     * @brief disable background copy
     *
     * @param[in] eid
     *
     * @return int
     */
    int disableBackgroundCopy(const EID& eid);
    /**
     * @brief install token on the device
     *
     * @param[in] eid
     * @param[in] token
     *
     * @return int
     */
    int installToken(const EID& eid, const Token& token);
    /**
     * @brief erase token on the device
     *
     * @param[in] eid
     *
     * @return int
     */
    int eraseToken(const EID& eid);
    /**
     * @brief query debug token status
     *
     * Query status with v3 first, then v2, and if both fail, query with v1.
     *
     * @param[in] eid
     * @return int - installation status
     */
    int queryDebugToken(const EID& eid);
    /**
     * @brief query debug token status with debug_token_query
     *
     * @param[in] eid
     * @return int - installation status
     */
    int queryDebugTokenV1(const EID& eid);
    /**
     * @brief query debug token status with debug_token_query_v2
     *
     * @param[in] eid
     * @return int - installation status
     */
    int queryDebugTokenV2(const EID& eid);

    /**
     * @brief query debug token status with debug_token_query_v3
     *
     * @param[in] eid
     * @return int - installation status
     */
    int queryDebugTokenV3(const EID& eid);
    /**
     * @brief Parse query v3 response.
     *
     * @param[in] rxBytes
     * @param[in] tokenInstallStatus
     * @param[in] installedTokenType
     * @param[in] eid
     *
     * @return int
     */
    int parseQueryV3Response(std::vector<std::string> rxBytes,
                             int& tokenInstallStatus, int& installedTokenType,
                             const EID& eid);
    /**
     * @brief Create a Log entry
     *
     * @param[in] messageID
     * @param[in] addData
     * @param[in] level
     *
     * @return void
     */
    void createLog(const std::string& messageID,
                   std::map<std::string, std::string>& addData, Level& level);
    /**
     * @brief debug token install for NSM endpoints.
     *
     * @param[in] tokens - token map
     * @return int
     */
    int nsmTokenInstall(TokenMap& tokens);

    /**
     * @brief debug token install for NSM endpoints V2 (TLV-based).
     *
     * @param[in] tokens - token map
     * @return int
     */
    int nsmTokenInstallV2(TokenMap& tokens);

    /**
     * @brief debug token erase for NSM endpoints.
     *
     * @return int
     */
    int nsmTokenErase();

    /**
     * @brief debug token erase for NSM endpoints V2.
     *
     * @return int
     */
    int nsmTokenEraseV2();

    /**
     * @brief Enumerate endpoints that support debug token over NSM.
     *
     * @param[in] nsmEndpoint - paths to be added by the function.
     *
     * @return int
     */
    int enumerateNsmDebugTokenEndpoints(NSMEndpoints& nsmEndpoint);

    /**
     * @brief Enumerate endpoints that support debug token over NSM V2.
     * Uses DebugTokenAction interface instead of DebugToken interface.
     *
     * @param[in] nsmEndpoint - paths to be added by the function.
     *
     * @return int
     */
    int enumerateNsmDebugTokenEndpointsV2(NSMEndpoints& nsmEndpoint);
    /**
     * @brief Handle async D-Bus call for NSM V2 token installation
     *
     * @param path NSM endpoint D-Bus object path
     * @param memfd Memory file descriptor containing token data
     * @return Async operation object path on success, empty string on failure
     */
    std::string handleAsyncCallInstallV2(const std::string& path, int memfd);
    /**
     * @brief Handle async D-Bus call for NSM V2 token erase
     *
     * @param path NSM endpoint D-Bus object path
     * @return Async operation object path on success, empty string on failure
     */
    std::string handleAsyncCallEraseV2(const std::string& path,
                                       uint32_t eraseType = EraseAll);

    /**
     * Helper function to make com.nvidia.DebugToken method calls
     * @param path The object path
     * @param methodName The name of the method to call
     * @param args The arguments to pass to the method
     * @return The async object path returned by the method
     */
    std::string makeDebugTokenMethodCall(
        const std::string& path, const std::string& methodName,
        const std::variant<std::monostate, std::string, std::vector<uint8_t>>&
            arg = std::monostate{});

    /**
     * Helper function to get async value
     * @param asyncPath The path of the async operation
     * @return The async value
     */
    NSMAsyncValue getAsyncValue(const std::string& path);

    /**
     * Helper function to log async operation errors
     * @param asyncPath The path of the async operation
     * @param methodName Name of the method to call
     * @param asyncStatus The status of the async operation
     */
    void logAsyncError(const std::string& asyncPath,
                       const std::string& methodName,
                       const std::string& asyncStatus);

    /**
     * Helper function to handle async calls with status monitoring
     * @param path The path of the async operation
     * @param methodName Name of the method to call
     * @param args The arguments to pass to the method
     * @return The async object path if operation succeeded, empty string
     * otherwise
     */
    std::string handleAsyncCall(
        const std::string& path, const std::string& methodName,
        const std::variant<std::monostate, std::string, std::vector<uint8_t>>&
            arg = std::monostate{});

    /**
     * Helper function to get token status for a given path
     * @param path The object path
     * @return The token status string if successful, empty string otherwise
     */
    std::string getTokenStatus(const std::string& path);
};
