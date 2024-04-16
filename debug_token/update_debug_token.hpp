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

#pragma once
#include "config.h"

#include "token_utility.hpp"

#include <fmt/format.h>

#include <fstream>
#include <map>

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
using SerialNumber = std::string;
using Token = std::vector<uint8_t>;
using DeviceMap = std::map<EID, SerialNumber>;
using TokenMap = std::map<SerialNumber, Token>;
using DeviceNameMap = std::map<EID, DeviceName>;
using MctpMedium = std::string;
using MctpBinding = std::string;
using Message = std::string;
using Resolution = std::string;
using MessageMapping = std::pair<Message, Resolution>;

using namespace dbus;
namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

static constexpr uint8_t mctpTypeSPDM = 0x5;
constexpr auto mctpPCIeService = "xyz.openbmc_project.MCTP.Control.PCIe";
constexpr auto mctpPath = "/xyz/openbmc_project/mctp";
constexpr auto objectMapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperIntfName = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mctpEndpointIntfName = "xyz.openbmc_project.MCTP.Endpoint";
constexpr auto uuidEndpointIntfName = "xyz.openbmc_project.Common.UUID";
constexpr auto mctpBindingIntfName = "xyz.openbmc_project.MCTP.Binding";
constexpr auto pldmService = "xyz.openbmc_project.PLDM";
constexpr auto pldmPath = "/";
constexpr auto pldmInventoryIntfName =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
const std::string mctpVdmUtilPath = "/usr/bin/mctp-vdm-util";
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};
static constexpr size_t queryStatusCodeByte =
    11; // 11 the byte from last is status code for debug token query
static constexpr size_t tokenInstallStatusByte =
    10; // 10 the byte from last is status code for debug token query
static constexpr size_t mctpDebugTokenQueryResponseLength =
    19; // Total length of MCTP respose : Header (9) + Data (10)

using Priority = int;
static std::unordered_map<MctpMedium, Priority> mediumPriority{
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 2},
};
static std::unordered_map<MctpBinding, Priority> bindingPriority{
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", 1},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", 2},
};


struct MctpEidInfo
{
    EID eid;
    MctpMedium medium;
    MctpBinding binding;

    friend bool operator<(MctpEidInfo const& lhs, MctpEidInfo const& rhs)
    {
        
        if (mediumPriority.at(lhs.medium) == mediumPriority.at(rhs.medium))
            return bindingPriority.at(lhs.binding) > bindingPriority.at(rhs.binding);
        else
            return mediumPriority.at(lhs.medium) > mediumPriority.at(rhs.medium);
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
    InstallInternalError
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
    EraseInternalError = 0x1
};

/* Debug token erase error code mapping for message registry */
static std::map<EraseErrorCodes, MessageMapping> eraseErrorMapping{
    {EraseErrorCodes::EraseInternalError,
     {"Debug Token Erase Internal Error for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}}};

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
    MCTPCommandInstallFailure,
    MCTPCommandEraseFailure,
    MCTPResponseInstallFailure,
    MCTPResponseEraseFailure
};

/* debug token common error code mapping for message registry */
static std::map<CommonErrorCodes, MessageMapping> debugTokenCommonErrorMapping{
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
    {CommonErrorCodes::MCTPCommandEraseFailure,
     {"Request to Erase Debug Token failed for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::MCTPResponseInstallFailure,
     {"Debug Token Install response is invalid for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}},
    {CommonErrorCodes::MCTPResponseEraseFailure,
     {"Debug Token Erase response is invalid for {}",
      "Retry the firmware update operation and if issue still persists reset"
      " the baseboard."}}};

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
     * @brief update token map which has mapping of serial number to EID
     *
     * @param[in] debugTokenPath - debug token file path
     * @param[in] tokens - token map
     *
     * @return int
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

  private:
    sdbusplus::bus::bus& bus;
    /* device map of EID to serial number */
    DeviceMap devices;
    /* map of UUID to EID */
    MctpInfo mctpInfo;

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
     * @brief discover MCTP end points
     *
     * @return int - status code
     */
    int discoverMCTPDevices();
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
     * @param[in] eid
     * @return int - installation status
     */
    int queryDebugToken(const EID& eid);
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
};
