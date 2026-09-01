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

#include "config.h"

#include "update_debug_token.hpp"

#include "tlv/tlv.h"

#include <endian.h>

#include <boost/container/flat_map.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

DebugTokenInstallStatus
    UpdateDebugToken::installDebugToken(const std::string& debugTokenPath)
{
    DebugTokenInstallStatus status =
        DebugTokenInstallStatus::DebugTokenInstallNone;
    TokenMap tokens;

    if (updateTokenMap(debugTokenPath, tokens) != 0)
    {
        log<level::ERR>("Error while parsing tokens");
        status = DebugTokenInstallStatus::DebugTokenInstallFailed;
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common,
            static_cast<int>(CommonErrorCodes::TokenParseFailure));
        return status;
    }

    size_t installedCount = 0;
    if (nsmTokenInstallV2(tokens, &installedCount) != 0)
    {
        log<level::ERR>("NSM V2 token installation failed");
        status = DebugTokenInstallStatus::DebugTokenInstallFailed;
        return status;
    }

    // A run in which no endpoint matched any token in the package is NOT a
    // success: nothing was sent to any device and no token was applied.
    // Reporting success here is what made the Redfish task complete green
    // while the token was never installed.
    if (installedCount == 0)
    {
        log<level::ERR>("No matching serial numbers for install token; "
                        "nothing was installed");
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common,
            static_cast<int>(CommonErrorCodes::NoMatchingDevice));
        status = DebugTokenInstallStatus::DebugTokenInstallNone;
        return status;
    }

    log<level::INFO>(("NSM V2 token installation succeeded for " +
                      std::to_string(installedCount) + " device(s)")
                         .c_str());
    status = DebugTokenInstallStatus::DebugTokenInstallSuccess;
    return status;
}

int UpdateDebugToken::eraseDebugToken()
{
    int status = eraseTokenSuccess;
    if (getErasePolicy() == "Manual")
    {
        log<level::INFO>("Erase policy set to manual, skipping operation.");
        std::map<std::string, std::string> addData;
        Level level = Level::Informational;
        addData["REDFISH_MESSAGE_ID"] = debugTokenEraseSkipped;
        addData["REDFISH_MESSAGE_ARGS"] = "erase policy is set to Manual";
        addData["namespace"] = "FWUpdate";
        createLog(debugTokenEraseSkipped, addData, level);
        // Nothing was erased; signal skip so the caller does not report
        // success.
        return eraseTokenSkipped;
    }

    if (nsmTokenEraseV2() != 0)
    {
        log<level::ERR>("NSM V2 token erase failed");
        status = eraseTokenFailed;
        return status;
    }
    else
    {
        log<level::INFO>("NSM V2 token erase succeeded");
    }
    return status;
}

std::string UpdateDebugToken::getErasePolicy()
{
    dbus::GetSubTreeResponse getSubTreeResponse{};
    const dbus::Interfaces ifaceList{erasePolicyIntfName};
    std::string policy;
    try
    {
        auto method = bus.new_method_call(objectMapperService, objectMapperPath,
                                          objectMapperIntfName, "GetSubTree");
        method.append(erasePolicyPath, 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("D-Bus error calling GetSubTree on ObjectMapper",
                        entry("ERROR=%s", e.what()));
        return policy;
    }
    if (getSubTreeResponse.size() == 0)
    {
        log<level::ERR>("No erase policy objects found");
        return policy;
    }
    if (getSubTreeResponse.size() != 1)
    {
        log<level::ERR>(
            "Only one erase policy object was expected, but more were found");
        return policy;
    }

    const auto& path = getSubTreeResponse[0].first;
    const auto& service = getSubTreeResponse[0].second[0].first;
    try
    {
        std::variant<std::string> policyProperty;
        auto method = bus.new_method_call(service.c_str(), path.c_str(),
                                          propertiesIntfName, "Get");
        method.append(erasePolicyIntfName, "Policy");
        auto reply = bus.call(method);
        reply.read(policyProperty);
        policy = std::get<std::string>(policyProperty);
        policy = policy.substr(policy.find_last_of('.') + 1);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("D-Bus error getting erase policy value",
                        entry("ERROR=%s", e.what()));
    }

    return policy;
}

std::set<dbus::Service> UpdateDebugToken::getMCTPServiceList()
{
    dbus::GetSubTreeResponse getSubTreeResponse{};
    std::set<dbus::Service> mctpServices{};
    const dbus::Interfaces ifaceList{mctpEndpointIntfName};
    try
    {
        auto method = bus.new_method_call(objectMapperService, objectMapperPath,
                                          objectMapperIntfName, "GetSubTree");
        method.append(mctpPath, 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("D-Bus error calling GetSubTree on ObjectMapper: ",
                        entry("ERROR=%s", e.what()));
    }

    for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
    {
        for (const auto& [service, interfaces] : mapperServiceMap)
        {
            mctpServices.insert(service);
        }
    }
    return mctpServices;
}

dbus::ObjectValueTree UpdateDebugToken::getMCTPManagedObjects()
{
    auto mctpServices = getMCTPServiceList();
    dbus::ObjectValueTree objects{};
    dbus::ObjectValueTree tmpObjects{};
    std::for_each(
        mctpServices.begin(), mctpServices.end(), [&](const auto& service) {
            try
            {
                auto method = bus.new_method_call(
                    service.c_str(), mctpPath,
                    "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
                auto reply = bus.call(method);
                reply.read(tmpObjects);
            }
            catch (const std::exception& e)
            {
                log<level::ERR>(
                    "D-Bus error calling Subtrees method on ObjectMapper: ",
                    entry("ERROR=%s", e.what()));
            }
            objects.insert(std::make_move_iterator(tmpObjects.begin()),
                           std::make_move_iterator(tmpObjects.end()));
            tmpObjects.clear();
        });
    return objects;
}

MctpEidInfo UpdateDebugToken::fetchEidInfoFromObject(
    const dbus::InterfaceMap& interfaces)
{
    EID eid{};
    MctpMedium mctpMedium{};
    MctpBinding mctpBinding{};
    SupportedMessageTypes supportedMsgTypes;
    bool enabled = false;
    const auto& eidProperties = interfaces.at(mctpEndpointIntfName);
    if (eidProperties.contains("EID") &&
        eidProperties.contains("SupportedMessageTypes"))
    {
        supportedMsgTypes = std::get<SupportedMessageTypes>(
            eidProperties.at("SupportedMessageTypes"));
        eid = std::get<uint8_t>(eidProperties.at("EID"));
    }

    if (eidProperties.contains("MediumType"))
    {
        mctpMedium = std::get<MctpMedium>(eidProperties.at("MediumType"));
    }

    if (interfaces.contains(mctpBindingIntfName))
    {
        const auto& bindingProperties = interfaces.at(mctpBindingIntfName);

        if (bindingProperties.contains("BindingType"))
        {
            mctpBinding =
                std::get<MctpBinding>(bindingProperties.at("BindingType"));
        }
    }
    // Check for Connectivity property in the new MCTP endpoint interface
    if (interfaces.contains(mctpEndpointEnableIntfName))
    {
        const auto& connectivityProperties =
            interfaces.at(mctpEndpointEnableIntfName);
        if (connectivityProperties.contains("Connectivity"))
        {
            std::string connectivity = std::get<std::string>(
                connectivityProperties.at("Connectivity"));
            enabled = (connectivity == "Available");
            if (!enabled)
            {
                log<level::INFO>(
                    ("MCTP endpoint connectivity is not available - EID=" +
                     std::to_string(eid) + ", connectivity=" + connectivity)
                        .c_str());
            }
        }
        else
        {
            log<level::ERR>(
                ("Failed to get MCTP endpoint Connectivity property - EID=" +
                 std::to_string(eid))
                    .c_str());
        }
    }
    else
    {
        log<level::ERR>(("Failed to get MCTP endpoint interface - EID=" +
                         std::to_string(eid))
                            .c_str());
    }
    return {eid, mctpMedium, mctpBinding, supportedMsgTypes, enabled};
}

bool UpdateDebugToken::checkSupportForSPDMandMCTPVDM(
    const SupportedMessageTypes& supportedMsgTypes, EID eid)
{
    if (std::find(supportedMsgTypes.begin(), supportedMsgTypes.end(),
                  mctpTypeSPDM) == supportedMsgTypes.end())
    {
        log<level::INFO>(("SPDM is not supported on EID=" +
                          std::to_string(eid) + ", skipping.")
                             .c_str());
        return false;
    }
    if (std::find(supportedMsgTypes.begin(), supportedMsgTypes.end(),
                  mctpTypeVDMIANA) == supportedMsgTypes.end())
    {
        log<level::INFO>(("MCTP VDM is not supported on EID=" +
                          std::to_string(eid) + ", skipping.")
                             .c_str());
        return false;
    }
    return true;
}

int UpdateDebugToken::discoverMCTPDevices()
{
    int status = 0;
    const auto& objects = getMCTPManagedObjects();
    if (objects.empty())
    {
        log<level::ERR>("Failed to fetch MCTP objects");
        status = -1;
        return status;
    }

    for (const auto& [objectPath, interfaces] : objects)
    {
        UUID uuid{};
        if (!interfaces.contains(mctpEndpointIntfName) or
            !interfaces.contains(uuidEndpointIntfName))
        {
            continue;
        }

        const auto& properties = interfaces.at(uuidEndpointIntfName);
        if (properties.contains("UUID"))
        {
            uuid = std::get<UUID>(properties.at("UUID"));
        }
        if (uuid.empty())
        {
            log<level::ERR>("MCTP EID object {PATH} has no UUID",
                            entry("PATH=%s", std::string(objectPath).c_str()));
            continue;
        }

        MctpEidInfo eidInfo = fetchEidInfoFromObject(interfaces);
        if ((eidInfo.medium.empty() and eidInfo.binding.empty()) or
            !eidInfo.enabled or
            !checkSupportForSPDMandMCTPVDM(eidInfo.supportedMsgTypes,
                                           eidInfo.eid))
        {
            continue;
        }

        // For devices having multiple EIDs only the faster medium is chosen for
        // transfer
        if (mctpInfo.find(uuid) == mctpInfo.end())
        {
            mctpInfo.emplace(uuid, eidInfo);
        }
        else
        {
            if (mctpInfo.at(uuid) < eidInfo)
            {
                mctpInfo[uuid] = eidInfo;
            }
        }
    }
    return status;
}

void UpdateDebugToken::updateDeviceMap(const dbus::InterfaceMap& interfaces,
                                       const std::string& deviceName)
{
    UUID uuid{};
    if (interfaces.contains(uuidEndpointIntfName))
    {
        const auto& properties = interfaces.at(uuidEndpointIntfName);
        if (properties.contains("UUID"))
        {
            uuid = std::get<std::string>(properties.at("UUID"));
        }
    }
    if (uuid.empty())
    {
        return;
    }
    if (interfaces.contains(pldmInventoryIntfName))
    {
        const auto& properties = interfaces.at(pldmInventoryIntfName);
        if (properties.contains("SerialNumber"))
        {
            SerialNumber serialNumber =
                std::get<SerialNumber>(properties.at("SerialNumber"));
            if (mctpInfo.find(uuid) != mctpInfo.end())
            {
                devices.emplace(mctpInfo[uuid].eid, serialNumber);
                deviceNameMap.emplace(mctpInfo[uuid].eid, deviceName);
            }
        }
    }
}

int UpdateDebugToken::updateEndPoints()
{
    int status = 0;
    dbus::ObjectValueTree objects{};

    if (discoverMCTPDevices() != 0)
    {
        log<level::ERR>("Error while discovering MCTP devices");
        return -1;
    }
    try
    {
        auto method = bus.new_method_call(pldmService, pldmPath,
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects");
        auto reply = bus.call(method);
        reply.read(objects);
        for (const auto& [objectPath, interfaces] : objects)
        {
            std::string deviceName = objectPath.filename();
            updateDeviceMap(interfaces, deviceName);
        }
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>("D-Bus error", entry("ERROR=%s", e.what()));
        return status;
    }
    return status;
}

int UpdateDebugToken::updateTokenMap(const std::string& debugTokenPath,
                                     TokenMap& tokens)
{
    int status = 0;

    if (!std::filesystem::exists(debugTokenPath))
    {
        log<level::ERR>("Debug token file does not exist");
        status = -1;
        return status;
    }

    std::ifstream debugTokenPackage(
        debugTokenPath, std::ios::binary | std::ios::in | std::ios::ate);
    if (!debugTokenPackage.is_open())
    {
        log<level::ERR>("Error opening debug token file");
        status = -1;
        return status;
    }
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
    auto headerInfo = getDebugTokenHeader(headerData, debugTokenPackage);
    if (!headerInfo)
    {
        log<level::ERR>("Invalid token header");
        status = -1;
        return status;
    }

    if (headerInfo->version == 2)
    {
        log<level::INFO>("TLV-based token detected (version 2.0)");
        debugTokenPackage.seekg(0, std::ios::end);
        std::streamsize fileSize = debugTokenPackage.tellg();
        debugTokenPackage.seekg(0, std::ios::beg);
        std::vector<uint8_t> fullFile(static_cast<size_t>(fileSize));
        debugTokenPackage.read(reinterpret_cast<char*>(fullFile.data()),
                               fileSize);
        return TokenUtility::parseTlvTokens(fullFile, headerInfo, tokens);
    }
    else
    {
        // Non-TLV format: use existing logic
        uint32_t tokenOffset = headerInfo->offsetToListOfStructs;
        for (uint16_t i = 0; i < headerInfo->numberOfRecords; i++)
        {
            std::vector<uint8_t> tokenData;
            std::vector<uint8_t> serialNumber;
            auto token = getNextDebugToken(debugTokenPackage, tokenOffset,
                                           tokenData, serialNumber);
            if (token)
            {
                if (token->structSize != tokenData.size())
                {
                    log<level::ERR>("Invalid token size");
                    status = -1;
                    return status;
                }
                std::string formattedSerialNumber =
                    TokenUtility::formatSerialNumber(serialNumber);
                std::string logEntry{"Read token - "};
                logEntry += std::string{token->identifier, 4};
                logEntry += " - ";
                logEntry += std::to_string(token->versionMajor);
                logEntry += ".";
                logEntry += std::to_string(token->versionMinor);
                logEntry += " - ";
                logEntry += formattedSerialNumber;
                log<level::INFO>(logEntry.c_str());
                tokens.emplace(formattedSerialNumber, tokenData);
                tokenOffset += token->structSize;
            }
            else
            {
                log<level::ERR>("Invalid debug token");
                status = -1;
                return status;
            }
        }
    }
    return status;
}

int UpdateDebugToken::installToken(const EID& eid, const Token& token)
{
    int status = 0;
    std::string command = "";
    std::stringstream tokenString;
    tokenString << std::hex;
    for (size_t x = 0; x < token.size(); x++)
    {
        tokenString << std::setw(2) << std::setfill('0') << (int)token[x]
                    << " ";
    }
    // form install command
    command += mctpVdmUtilPath;
    command += " -c debug_token_install ";
    command += "-t " + std::to_string(eid);
    command += " " + tokenString.str();
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>(("Error while running install token command for EID=" +
                         std::to_string(eid))
                            .c_str());
        status = static_cast<int>(CommonErrorCodes::MCTPCommandInstallFailure);
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common, status, deviceName);
        return status;
    }
    std::string deviceName;
    if (deviceNameMap.contains(eid))
    {
        deviceName = deviceNameMap[eid];
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        if (rxBytes.size() > 0)
        {
            // last byte is status code
            status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
        }
        else
        {
            status =
                static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure);
            createMessageRegistryResourceErrors(
                resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
                OperationType::Common, status, deviceName);
            log<level::ERR>(("Error while parsing mctp response for EID=" +
                             std::to_string(eid))
                                .c_str());
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status = static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure);
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common, status, deviceName);
        log<level::ERR>(
            ("Error while getting status code for EID=" + std::to_string(eid))
                .c_str());
        return status;
    }
    if (status != static_cast<int>(InstallErrorCodes::InstallSuccess))
    {
        log<level::ERR>(("Error while installing token: " + commandOut +
                         " for EID=" + std::to_string(eid))
                            .c_str());

        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::TokenInstall, status, deviceName);
        // enable background copy since token installation is failed
        // which is the default setting
        if (enableBackgroundCopy(eid) != 0)
        {
            log<level::ERR>(
                ("Enable BackgroundCopy failed for EID " + std::to_string(eid))
                    .c_str());
        }
        else
        {
            log<level::INFO>(
                ("Enable BackgroundCopy success for EID " + std::to_string(eid))
                    .c_str());
        }
    }
    return status;
}

int UpdateDebugToken::eraseToken(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c debug_token_erase ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    std::string deviceName;
    if (deviceNameMap.contains(eid))
    {
        deviceName = deviceNameMap[eid];
    }
    if (retCode != 0)
    {
        log<level::ERR>(("Error while running erase token command for EID=" +
                         std::to_string(eid))
                            .c_str());
        status = static_cast<int>(CommonErrorCodes::MCTPCommandEraseFailure);
        createMessageRegistryResourceErrors(
            debugTokenEraseFailed, DEBUG_TOKEN_ERASE_NAME,
            OperationType::Common, status, deviceName);
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        if (rxBytes.size() > 0)
        {
            status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
        }
        else
        {
            log<level::ERR>(("Error while parsing MCTP response for EID=" +
                             std::to_string(eid))
                                .c_str());
            status =
                static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure);
            std::string deviceName;
            if (deviceNameMap.contains(eid))
            {
                deviceName = deviceNameMap[eid];
            }
            createMessageRegistryResourceErrors(
                debugTokenEraseFailed, DEBUG_TOKEN_ERASE_NAME,
                OperationType::Common, status, deviceName);
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status = static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure);
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
            OperationType::Common, status, deviceName);
        log<level::ERR>(
            ("Error while getting status code for EID=" + std::to_string(eid))
                .c_str());
        return status;
    }
    if (status != static_cast<int>(EraseErrorCodes::EraseSuccess))
    {
        log<level::ERR>(("Error while erasing token: " + commandOut +
                         " for EID=" + std::to_string(eid))
                            .c_str());
        status = -1;
        createMessageRegistryResourceErrors(
            debugTokenEraseFailed, DEBUG_TOKEN_ERASE_NAME,
            OperationType::TokenErase,
            static_cast<int>(EraseErrorCodes::EraseInternalError), deviceName);
        // disable background copy since token erase is failed
        // this will avoid debug image getting copied to both the partition
        if (disableBackgroundCopy(eid) != 0)
        {
            log<level::ERR>(
                ("Disable BackgroundCopy failed for EID " + std::to_string(eid))
                    .c_str());
        }
        else
        {
            log<level::INFO>(("Disable BackgroundCopy success for EID " +
                              std::to_string(eid))
                                 .c_str());
        }
    }
    return status;
}

int UpdateDebugToken::disableBackgroundCopy(const EID& eid)
{
    int status = 0;
    std::string command = "";
    command += mctpVdmUtilPath;
    command += " -c background_copy_disable ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        status = -1;
        log<level::ERR>(
            ("Error while running background copy disable command for EID=" +
             std::to_string(eid))
                .c_str());
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        if (rxBytes.size() > 0)
        {
            // last byte is status code
            status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
        }
        else
        {
            status = static_cast<int>(
                BackgroundCopyErrorCodes::BackgroundDisableFail);
            log<level::ERR>(("Error while parsing MCTP response for EID=" +
                             std::to_string(eid))
                                .c_str());
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status =
            static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopyFailed);
        log<level::ERR>(
            ("Error while getting status code for EID=" + std::to_string(eid))
                .c_str());
    }
    if (status !=
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopySuccess))
    {
        log<level::ERR>(("Error while disabling background copy: " +
                         commandOut + " for EID=" + std::to_string(eid))
                            .c_str());
        status = -1;
    }
    return status;
}

int UpdateDebugToken::enableBackgroundCopy(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c background_copy_enable ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>(
            ("Error while running background copy enable command for EID=" +
             std::to_string(eid))
                .c_str());
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        if (rxBytes.size() > 0)
        {
            // last byte is status code
            status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
        }
        else
        {
            status = static_cast<int>(
                BackgroundCopyErrorCodes::BackgroundEnableFail);
            log<level::ERR>(("Error while parsing MCTP response for EID=" +
                             std::to_string(eid))
                                .c_str());
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status =
            static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopyFailed);
        log<level::ERR>(
            ("Error while getting status code for EID=" + std::to_string(eid))
                .c_str());
    }
    if (status !=
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopySuccess))
    {
        log<level::ERR>(("Error while enabling background copy: " + commandOut +
                         " for EID=" + std::to_string(eid))
                            .c_str());
        status = -1;
    }
    return status;
}

int UpdateDebugToken::queryDebugTokenV1(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form debug_token_query command
    command += mctpVdmUtilPath;
    command += " -c debug_token_query ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>(
            ("Error while running debug token query command for EID=" +
             std::to_string(eid))
                .c_str());
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        if (rxBytes.size() != mctpDebugTokenQueryResponseLengthV1)
        {
            status = -1;
            log<level::ERR>(
                ("Debug token query command response size is invalid for EID=" +
                 std::to_string(eid))
                    .c_str());
            return status;
        }
        status = std::stoi(rxBytes[mctpCompletionCodeByte], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting status code for EID=" + std::to_string(eid))
                .c_str());
    }
    if (status != 0)
    {
        log<level::ERR>(("Error while parsing debug token query output: " +
                         commandOut + " for EID=" + std::to_string(eid))
                            .c_str());
        status = -1;
        return status;
    }
    try
    {
        // 10 the byte from last is token installation status
        auto tokenInstallStatus =
            std::stoi(rxBytes[tokenInstallStatusByteV1], nullptr, 16);
        if (tokenInstallStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled))
        {
            status = static_cast<int>(
                DebugTokenQueryErrorCodes::DebugTokenInstalled);
        }
        else
        {
            status = static_cast<int>(
                DebugTokenQueryErrorCodes::DebugTokenNotInstalled);
        }
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting token installation status for EID=" +
             std::to_string(eid))
                .c_str());
    }
    return status;
}

int UpdateDebugToken::queryDebugTokenV2(const EID& eid)
{
    int status = 0;
    int tokenInstallStatus = 0;
    int installedTokenType = 0;
    std::string command = "";
    // form debug_token_query_v2 command
    command += mctpVdmUtilPath;
    command += " -c debug_token_query_v2 ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>(
            ("Error while running debug_token_query_v2 command for EID=" +
             std::to_string(eid))
                .c_str());
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    status = parseQueryV2Response(rxBytes, tokenInstallStatus,
                                  installedTokenType, eid);
    if (status == 0)
    {
        log<level::INFO>(("debug_token_query_v2 Token Install Status: " +
                          std::to_string(tokenInstallStatus) +
                          " for EID=" + std::to_string(eid))
                             .c_str());
        if (tokenInstallStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled))
        {
            log<level::INFO>(("debug_token_query_v2 Installed Token Type: " +
                              std::to_string(installedTokenType) +
                              " for EID=" + std::to_string(eid))
                                 .c_str());
        }
        return tokenInstallStatus;
    }
    if (status == -1)
    {
        // Dump response in case of failure.
        log<level::ERR>(commandOut.c_str());
    }
    return status;
}

int UpdateDebugToken::parseQueryV2Response(std::vector<std::string> rxBytes,
                                           int& tokenInstallStatus,
                                           int& installedTokenType,
                                           const EID& eid)
{
    int status = 0;
    try
    {
        if (rxBytes.size() != mctpDebugTokenQueryResponseLengthV2)
        {
            if (rxBytes.size() > mctpCompletionCodeByte)
            {
                status =
                    std::stoi(rxBytes[mctpCompletionCodeByte], nullptr, 16);
                log<level::ERR>(
                    ("debug_token_query_v2 command failed with code: " +
                     std::to_string(status) + " for EID=" + std::to_string(eid))
                        .c_str());
            }
            else
            {
                status = -1;
                log<level::ERR>(
                    ("debug_token_query_v2 command response size is invalid for EID=" +
                     std::to_string(eid))
                        .c_str());
            }
            return status;
        }
        status = std::stoi(rxBytes[mctpCompletionCodeByte], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting status code from debug_token_query_v2 for EID=" +
             std::to_string(eid))
                .c_str());
    }
    if (status != static_cast<int>(MCTPCompletionCodes::Success))
    {
        log<level::ERR>(("Error while parsing debug token query v2 output: " +
                         std::to_string(status))
                            .c_str());
        status = -1;
        return status;
    }
    try
    {
        tokenInstallStatus =
            std::stoi(rxBytes[tokenInstallStatusByteV2], nullptr, 16);
        if (tokenInstallStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled))
        {
            installedTokenType = 0;
            uint8_t shift = 0;
            for (int idx = tokenTypeByteStartV2; idx <= tokenTypeByteEndV2;
                 idx++)
            {
                installedTokenType |=
                    (((uint32_t)std::stoi(rxBytes[idx], nullptr, 16)) << shift);
                shift += 8;
            }
        }
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting token installation status for EID=" +
             std::to_string(eid))
                .c_str());
    }
    return status;
}

int UpdateDebugToken::queryDebugTokenV3(const EID& eid)
{
    int status = 0;
    int tokenInstallStatus = 0;
    int installedTokenType = 0;
    std::string command = "";
    // form debug_token_query_v3 command
    command += mctpVdmUtilPath;
    command += " -c debug_token_query_v3 ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>(
            ("Error while running debug_token_query_v3 command for EID=" +
             std::to_string(eid))
                .c_str());
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    status = parseQueryV3Response(rxBytes, tokenInstallStatus,
                                  installedTokenType, eid);
    if (status == 0)
    {
        log<level::INFO>(("debug_token_query_v3 Token Install Status: " +
                          std::to_string(tokenInstallStatus) +
                          " for EID=" + std::to_string(eid))
                             .c_str());
        if (tokenInstallStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled))
        {
            log<level::INFO>(("debug_token_query_v3 Installed Token Type: " +
                              std::to_string(installedTokenType) +
                              " for EID=" + std::to_string(eid))
                                 .c_str());
        }
        return tokenInstallStatus;
    }
    if (status == -1)
    {
        // Dump response in case of failure.
        log<level::ERR>(commandOut.c_str());
    }
    return status;
}

int UpdateDebugToken::parseQueryV3Response(std::vector<std::string> rxBytes,
                                           int& tokenInstallStatus,
                                           int& installedTokenType,
                                           const EID& eid)
{
    int status = 0;
    try
    {
        if (rxBytes.size() != mctpDebugTokenQueryResponseLengthV3)
        {
            if (rxBytes.size() > mctpCompletionCodeByte)
            {
                status =
                    std::stoi(rxBytes[mctpCompletionCodeByte], nullptr, 16);
                log<level::ERR>(
                    ("debug_token_query_v3 command failed with code: " +
                     std::to_string(status) + " for EID=" + std::to_string(eid))
                        .c_str());
            }
            else
            {
                status = -1;
                log<level::ERR>(
                    ("debug_token_query_v3 command response size is invalid for EID=" +
                     std::to_string(eid))
                        .c_str());
            }
            return status;
        }
        status = std::stoi(rxBytes[mctpCompletionCodeByte], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting status code from debug_token_query_v3 for EID=" +
             std::to_string(eid))
                .c_str());
    }
    if (status != static_cast<int>(MCTPCompletionCodes::Success))
    {
        log<level::ERR>(("Error while parsing debug token query v3 output: " +
                         std::to_string(status))
                            .c_str());
        status = -1;
        return status;
    }
    try
    {
        tokenInstallStatus =
            std::stoi(rxBytes[tokenInstallStatusByteV3], nullptr, 16);
        if (tokenInstallStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled))
        {
            installedTokenType = 0;
            uint8_t shift = 0;
            for (int idx = tokenTypeByteStartV3; idx <= tokenTypeByteEndV3;
                 idx++)
            {
                installedTokenType |=
                    (((uint32_t)std::stoi(rxBytes[idx], nullptr, 16)) << shift);
                shift += 8;
            }
        }
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>(
            ("Error while getting token installation status for EID=" +
             std::to_string(eid))
                .c_str());
    }
    return status;
}

int UpdateDebugToken::queryDebugToken(const EID& eid)
{
    int status = 0;
    status = queryDebugTokenV3(eid);
    // If v3 fails, try v2
    if (status !=
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled) &&
        status !=
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled))
    {
        status = queryDebugTokenV2(eid);
        // If v2 fails, try v1
        if (status != static_cast<int>(
                          DebugTokenQueryErrorCodes::DebugTokenInstalled) &&
            status != static_cast<int>(
                          DebugTokenQueryErrorCodes::DebugTokenNotInstalled))
        {
            status = queryDebugTokenV1(eid);
        }
    }
    return status;
}

void UpdateDebugToken::createLog(const std::string& messageID,
                                 std::map<std::string, std::string>& addData,
                                 Level& level)
{
    static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
    static constexpr auto logInterface = "xyz.openbmc_project.Logging.Create";
    static constexpr auto service = "xyz.openbmc_project.Logging";
    try
    {
        auto severity = LoggingServer::convertForMessage(level);
        auto method =
            bus.new_method_call(service, logObjPath, logInterface, "Create");
        method.append(messageID, severity, addData);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Failed to create D-Bus log entry for message registry",
                        entry("ERROR=%s", e.what()));
    }
    return;
}

void UpdateDebugToken::createMessageRegistry(const std::string& messageID,
                                             const std::string& compName,
                                             const std::string& compVersion)
{
    std::map<std::string, std::string> addData;
    Level level = Level::Informational;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    if (messageID == updateSuccessful)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compName + "," + compVersion);
    }
    else
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compVersion + "," + compName);
        level = Level::Critical;
    }
    // use separate container for fwupdate message registry
    addData["namespace"] = "FWUpdate";
    createLog(messageID, addData, level);
    return;
}

std::optional<std::tuple<std::string, std::string>>
    UpdateDebugToken::getMessage(const OperationType operationType,
                                 const int errorCode,
                                 const std::string deviceName)
{
    Message errorMessage;
    Resolution resolution;
    switch (operationType)
    {
        case OperationType::TokenInstall:
            if (installErrorMapping.contains(
                    static_cast<InstallErrorCodes>(errorCode)))
            {
                auto errorCodeSearch = installErrorMapping.find(
                    static_cast<InstallErrorCodes>(errorCode));
                errorMessage =
                    formatMessage(errorCodeSearch->second.first, deviceName);
                resolution = errorCodeSearch->second.second;
                return {{errorMessage, resolution}};
            }
            break;
        case OperationType::TokenErase:
            if (eraseErrorMapping.contains(
                    static_cast<EraseErrorCodes>(errorCode)))
            {
                auto errorCodeSearch = eraseErrorMapping.find(
                    static_cast<EraseErrorCodes>(errorCode));
                errorMessage =
                    formatMessage(errorCodeSearch->second.first, deviceName);
                resolution = errorCodeSearch->second.second;
                return {{errorMessage, resolution}};
            }
            break;
        case OperationType::BackgroundCopy:
            if (backgroundCopyErrorMapping.contains(
                    static_cast<BackgroundCopyErrorCodes>(errorCode)))
            {
                auto errorCodeSearch = backgroundCopyErrorMapping.find(
                    static_cast<BackgroundCopyErrorCodes>(errorCode));
                errorMessage =
                    formatMessage(errorCodeSearch->second.first, deviceName);
                resolution = errorCodeSearch->second.second;
                return {{errorMessage, resolution}};
            }
            break;
        case OperationType::Common:
            if (debugTokenCommonErrorMapping.contains(
                    static_cast<CommonErrorCodes>(errorCode)))
            {
                auto errorCodeSearch = debugTokenCommonErrorMapping.find(
                    static_cast<CommonErrorCodes>(errorCode));
                errorMessage =
                    formatMessage(errorCodeSearch->second.first, deviceName);
                resolution = errorCodeSearch->second.second;
                return {{errorMessage, resolution}};
            }
            break;
        default:
            log<level::ERR>(
                "No mapping found for command.",
                entry("OPERATIONTYPE %ld", (unsigned)operationType));
            break;
    }
    return {};
}

void UpdateDebugToken::createMessageRegistryResourceErrors(
    const std::string& messageID, const std::string& componentName,
    const OperationType& operationType, const int& errorCode,
    const std::string deviceName)
{
    std::optional<std::tuple<std::string, std::string>> message =
        getMessage(operationType, errorCode, deviceName);
    if (message)
    {
        std::map<std::string, std::string> addData;
        Level level = Level::Informational;
        addData["REDFISH_MESSAGE_ID"] = messageID;
        addData["REDFISH_MESSAGE_ARGS"] =
            (componentName + "," + std::get<0>(*message));
        if (messageID == resourceErrorsDetected)
        {
            level = Level::Critical;
        }
        // use separate container for fwupdate message registry
        addData["namespace"] = "FWUpdate";
        std::string resolution = std::get<1>(*message);
        if (!resolution.empty())
        {
            addData["xyz.openbmc_project.Logging.Entry.Resolution"] =
                resolution;
        }
        createLog(messageID, addData, level);
    }
    else
    {
        log<level::ERR>("Unable to log message registry.",
                        entry("DeviceName: %s", deviceName.c_str()));
    }
    return;
}