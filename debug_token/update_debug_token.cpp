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

#include "config.h"

#include "update_debug_token.hpp"

#include <filesystem>

DebugTokenInstallStatus
    UpdateDebugToken::installDebugToken(const std::string& debugTokenPath)
{
    DebugTokenInstallStatus status =
        DebugTokenInstallStatus::DebugTokenInstallNone;
    int queryStatus = static_cast<int>(
        DebugTokenQueryErrorCodes::DebugTokenNotInstalled);
    TokenMap tokens;
    if (updateEndPoints() != 0)
    {
        log<level::ERR>("discovery failed");
        status = DebugTokenInstallStatus::DebugTokenInstallFailed;
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common,
            static_cast<int>(CommonErrorCodes::MCTPDiscoveryFailed));
        return status;
    }
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
    for (auto& device : devices)
    {
        if (tokens.find(device.second) != tokens.end())
        {
            queryStatus = queryDebugToken(device.first);
            if(queryStatus < 0)
            {
                continue;
            }
            else if (queryStatus == static_cast<int>(
                    DebugTokenQueryErrorCodes::DebugTokenInstalled))
            {
                log<level::ERR>(("debug token already installed on EID " +
                                 std::to_string(device.first))
                                    .c_str());
                // skip install token for this device since token is already
                // installed
                if (status != DebugTokenInstallStatus::DebugTokenInstallFailed)
                {
                    status = DebugTokenInstallStatus::DebugTokenInstallSuccess;
                }
                std::string deviceName;
                if (deviceNameMap.contains(device.first))
                {
                    deviceName = deviceNameMap[device.first];
                }
                continue;
            }
            if (disableBackgroundCopy(device.first) != 0)
            {
                log<level::ERR>(("Disable BackgroundCopy failed for EID " +
                                 std::to_string(device.first))
                                    .c_str());
                status = DebugTokenInstallStatus::DebugTokenInstallFailed;
                std::string deviceName;
                if (deviceNameMap.contains(device.first))
                {
                    deviceName = deviceNameMap[device.first];
                }
                createMessageRegistryResourceErrors(
                    resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
                    OperationType::BackgroundCopy,
                    static_cast<int>(
                        BackgroundCopyErrorCodes::BackgroundDisableFail),
                    deviceName);
                // abort install token for this device if disabling background
                // copy is failed
                continue;
            }
            else
            {
                log<level::INFO>(("Disable BackgroundCopy success for EID " +
                                  std::to_string(device.first))
                                     .c_str());
            }
            int installErrorCode =
                installToken(device.first, tokens[device.second]);
            if (static_cast<InstallErrorCodes>(installErrorCode) !=
                InstallErrorCodes::InstallSuccess)
            {
                log<level::ERR>(("DebugToken Install failed for EID " +
                                 std::to_string(device.first))
                                    .c_str());
                status = DebugTokenInstallStatus::DebugTokenInstallFailed;
            }
            else
            {
                log<level::INFO>(("DebugToken Install success for EID " +
                                  std::to_string(device.first))
                                     .c_str());
                if (status != DebugTokenInstallStatus::DebugTokenInstallFailed)
                {
                    status = DebugTokenInstallStatus::DebugTokenInstallSuccess;
                }
            }
        }
    }
    return status;
}

int UpdateDebugToken::eraseDebugToken()
{
    int status = 0;
    int queryStatus = static_cast<int>(
        DebugTokenQueryErrorCodes::DebugTokenNotInstalled);
    if (updateEndPoints() != 0)
    {
        log<level::ERR>("discovery failed");
        status = -1;
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
            OperationType::Common,
            static_cast<int>(CommonErrorCodes::MCTPDiscoveryFailed));
        return status;
    }
    for (auto& [uuid, mctpEidInfo] : mctpInfo)
    {
        queryStatus = queryDebugToken(mctpEidInfo.eid);
        if(queryStatus < 0 || queryStatus ==
            static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled))
        {
            // skip erase token for this device since token is not installed OR
            // there was an error with querying debug token status
            continue;
        }
        if (eraseToken(mctpEidInfo.eid) != 0)
        {
            log<level::ERR>(("DebugToken Erase failed for EID=" +
                             std::to_string(mctpEidInfo.eid))
                                .c_str());
            status = -1;
            return status;
        }
        else
        {
            log<level::INFO>(("DebugToken Erase success for EID=" +
                              std::to_string(mctpEidInfo.eid))
                                 .c_str());
        }
        if (enableBackgroundCopy(mctpEidInfo.eid) != 0)
        {
            log<level::ERR>(("Enable BackgroundCopy failed for EID " +
                             std::to_string(mctpEidInfo.eid))
                                .c_str());
            status = -1;
            std::string deviceName;
            if (deviceNameMap.contains(mctpEidInfo.eid))
            {
                deviceName = deviceNameMap[mctpEidInfo.eid];
            }
            createMessageRegistryResourceErrors(
                resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
                OperationType::BackgroundCopy,
                static_cast<int>(
                    BackgroundCopyErrorCodes::BackgroundEnableFail),
                deviceName);
        }
        else
        {
            log<level::INFO>(("Enable BackgroundCopy success for EID " +
                              std::to_string(mctpEidInfo.eid))
                                 .c_str());
        }
    }
    return status;
}

std::set<dbus::Service> UpdateDebugToken::getMCTPServiceList()
{
    dbus::GetSubTreeResponse getSubTreeResponse{};
    std::set<dbus::Service> mctpServices{};
    const dbus::Interfaces ifaceList{mctpEndpointIntfName};
    try
    {
        auto method = bus.new_method_call(objectMapperService, objectMapperPath,
                                          objectMapperIntfName,
                                          "GetSubTree");
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
    std::for_each(mctpServices.begin(), mctpServices.end(),
        [&](const auto& service)
        {
            try
            {
                auto method = bus.new_method_call(service.c_str(), mctpPath,
                        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
                auto reply = bus.call(method);
                reply.read(tmpObjects);
            }
            catch (const std::exception& e)
            {
                log<level::ERR>("D-Bus error calling Subtrees method on ObjectMapper: ", entry("ERROR=%s", e.what()));
            }
            objects.insert(std::make_move_iterator(tmpObjects.begin()),
                    std::make_move_iterator(tmpObjects.end()));
            tmpObjects.clear();
        });
    return objects;

}

MctpEidInfo UpdateDebugToken::fetchEidInfoFromObject(const dbus::InterfaceMap& interfaces)
{
    EID eid{};
    MctpMedium mctpMedium{};
    MctpBinding mctpBinding{};
    SupportedMessageTypes supportedMsgTypes;
    const auto& eidProperties = interfaces.at(mctpEndpointIntfName);
    if (eidProperties.contains("EID") &&
        eidProperties.contains("SupportedMessageTypes"))
    {
        supportedMsgTypes = std::get<SupportedMessageTypes>(
            eidProperties.at("SupportedMessageTypes"));
        eid = std::get<size_t>(eidProperties.at("EID"));
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
            mctpBinding = std::get<MctpBinding>(bindingProperties.at("BindingType"));
        }
    }
    return {eid, mctpMedium, mctpBinding, supportedMsgTypes};

}

bool UpdateDebugToken::checkSupportForSPDMandMCTPVDM(
    const SupportedMessageTypes& supportedMsgTypes, EID eid)
{
    if (std::find(supportedMsgTypes.begin(), supportedMsgTypes.end(),
                  mctpTypeSPDM) == supportedMsgTypes.end())
    {
        log<level::INFO>("SPDM not supported on EID={EID}, skipping.",
                         entry("EID=%d", eid));
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
        if ((eidInfo.medium.empty() and eidInfo.binding.empty()) ||
            !checkSupportForSPDMandMCTPVDM(eidInfo.supportedMsgTypes,
                                           eidInfo.eid))
        {
            continue;
        }

        // For devices having multiple EIDs only the faster medium is chosen for transfer
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
    std::ifstream debugTokenPackage(
        debugTokenPath, std::ios::binary | std::ios::in | std::ios::ate);
    uint32_t packageSize = debugTokenPackage.tellg();
    if (!debugTokenPackage.is_open())
    {
        log<level::ERR>("Error while opening the file");
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
    uint32_t tokenOffset = headerInfo->offsetToListOfStructs;
    for (uint16_t i = 0; i < headerInfo->numberOfRecords; i++)
    {
        Token token(sizeof(DebugToken));
        if ((tokenOffset + sizeof(DebugToken)) > packageSize)
        {
            log<level::ERR>("Token offset out of range");
            break;
        }
        auto debugTokenInfo =
            gextNextDebugToken(token, tokenOffset, debugTokenPackage);
        if (debugTokenInfo)
        {
            std::stringstream serialNumber;
            serialNumber << std::hex;
            for (size_t x = 0; x < sizeof(debugTokenInfo->serialNumber); x++)
            {
                serialNumber << std::uppercase << std::setw(2)
                             << std::setfill('0')
                             << (int)debugTokenInfo->serialNumber[x];
            }
            tokens.emplace(("0x" + serialNumber.str()), token);
        }
        else
        {
            log<level::ERR>(
                "Invalid debug token"); // skip and move to next token
        }
        tokenOffset += sizeof(DebugToken);
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
        log<level::ERR>("Error while running install token command");
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
            std::string deviceName;
            if (deviceNameMap.contains(eid))
            {
                deviceName = deviceNameMap[eid];
            }
            createMessageRegistryResourceErrors(
                resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
                OperationType::Common, status, deviceName);
            log<level::ERR>("Error while parsing mctp response");
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status = static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure);
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_INSTALL_NAME,
            OperationType::Common, status, deviceName);
        log<level::ERR>("Error while getting status code");
        return status;
    }
    if (status != static_cast<int>(InstallErrorCodes::InstallSuccess))
    {
        log<level::ERR>(
            ("Error while installing token: " + commandOut).c_str());
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
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
    if (retCode != 0)
    {
        log<level::ERR>("Error while running erase token command");
        status = static_cast<int>(CommonErrorCodes::MCTPCommandEraseFailure);
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
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
            log<level::ERR>("Error while parsing MCTP response");
            status =
                static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure);
            std::string deviceName;
            if (deviceNameMap.contains(eid))
            {
                deviceName = deviceNameMap[eid];
            }
            createMessageRegistryResourceErrors(
                resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
                OperationType::Common, status, deviceName);
            return status;
        }
    }
    catch (const std::exception& e)
    {
        status = static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure);
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
            OperationType::Common, status, deviceName);
        log<level::ERR>("Error while getting status code");
        return status;
    }
    if (status != static_cast<int>(EraseErrorCodes::EraseSuccess))
    {
        log<level::ERR>(("Error while erasing token: " + commandOut).c_str());
        status = -1;
        std::string deviceName;
        if (deviceNameMap.contains(eid))
        {
            deviceName = deviceNameMap[eid];
        }
        createMessageRegistryResourceErrors(
            resourceErrorsDetected, DEBUG_TOKEN_ERASE_NAME,
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
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c background_copy_disable ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        status = -1;
        log<level::ERR>("Error while running background copy disable command");
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status =
            static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopyFailed);
        log<level::ERR>("Error while getting status code");
    }
    if (status !=
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopySuccess))
    {
        log<level::ERR>(
            ("Error while disabling background copy: " + commandOut).c_str());
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
        log<level::ERR>("Error while running background copy enable command");
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status =
            static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopyFailed);
        log<level::ERR>("Error while getting status code");
    }
    if (status !=
        static_cast<int>(BackgroundCopyErrorCodes::BackgroundCopySuccess))
    {
        log<level::ERR>(
            ("Error while enabling background copy: " + commandOut).c_str());
        status = -1;
    }
    return status;
}

int UpdateDebugToken::queryDebugToken(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c debug_token_query ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>("Error while running debug token query command");
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        if (rxBytes.size() != mctpDebugTokenQueryResponseLength)
        {
            status = -1;
            log<level::ERR>(
                "Debug token query command response size is invalid.");
            return status;
        }
        // 11 the byte from last is status code
        status = std::stoi(rxBytes[rxBytes.size() - queryStatusCodeByte],
                            nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>("Error while getting status code");
    }
    if (status != 0)
    {
        log<level::ERR>(
            ("Error while parsing debug token query output: " + commandOut)
                .c_str());
        status = -1;
        return status;
    }
    try
    {
        // 10 the byte from last is token installation status
        auto tokenInstallStatus =
            std::stoi(rxBytes[rxBytes.size() - tokenInstallStatusByte],
                        nullptr, 16);
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
        log<level::ERR>("Error while getting token installation status");
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