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

#include <boost/container/flat_map.hpp>

#include <chrono>
#include <filesystem>

int UpdateDebugToken::enumerateNsmDebugTokenEndpoints(
    NSMEndpoints& nsmEndpoints)
{
    dbus::GetSubTreeResponse objects{};
    const dbus::Interfaces ifaceList{nsmDebugTokenIntfName};
    try
    {
        auto method = bus.new_method_call(objectMapperService, objectMapperPath,
                                          objectMapperIntfName, "GetSubTree");
        method.append(nsmDebugTokenPath, 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(objects);
        for ([[maybe_unused]] const auto& [objectPath, mapperServiceMap] :
             objects)
        {
            nsmEndpoints.push_back(objectPath);
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            ("NSM Command Exception DBUS_ERROR: " + std::string(e.what()))
                .c_str());
        return -1;
    }
    return 0;
}

std::string UpdateDebugToken::makeDebugTokenMethodCall(
    const std::string& path, const std::string& methodName,
    const std::variant<std::monostate, std::string, std::vector<uint8_t>>& arg)
{
    try
    {
        auto method =
            bus.new_method_call(nsmService, path.c_str(), nsmDebugTokenIntfName,
                                methodName.c_str());
        if (methodName == "GetStatus")
        {
            method.append(std::get<std::string>(arg));
        }
        else if (methodName == "InstallToken")
        {
            method.append(std::get<std::vector<uint8_t>>(arg));
        }
        auto reply = bus.call(method);
        sdbusplus::message::object_path asyncPath;
        reply.read(asyncPath);
        return asyncPath;
    }
    catch (const std::exception& e)
    {
        log<level::ERR>((path + ": " + methodName +
                         " failed with status: " + std::string(e.what()))
                            .c_str());
        return "";
    }
}

NSMAsyncValue UpdateDebugToken::getAsyncValue(const std::string& path)
{
    auto method = bus.new_method_call(nsmService, path.c_str(),
                                      propertiesIntfName, "Get");
    method.append(nsmAsyncValueIntfName, "Value");
    auto reply = bus.call(method);
    std::variant<NSMAsyncValue> value;
    reply.read(value);
    return std::get<NSMAsyncValue>(value);
}

void UpdateDebugToken::logAsyncError(const std::string& path,
                                     const std::string& methodName,
                                     const std::string& asyncStatus)
{
    try
    {
        auto errorValue = getAsyncValue(path);
        auto& [errorCode, errorMessage] = std::get<NSMErrorTuple>(errorValue);
        log<level::ERR>((path + ": " + methodName +
                         " failed with status: " + asyncStatus +
                         ", error code: " + std::to_string(errorCode) +
                         ", message: " + errorMessage)
                            .c_str());
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            (path + ": " + methodName + " failed with status: " + asyncStatus +
             ", error details not available: " + std::string(e.what()))
                .c_str());
    }
}

std::string UpdateDebugToken::handleAsyncCall(
    const std::string& path, const std::string& methodName,
    const std::variant<std::monostate, std::string, std::vector<uint8_t>>& arg)
{
    std::string asyncObjectPath, status;
    std::unique_ptr<sdbusplus::bus::match_t> statusMatch;
    std::string matchRule =
        sdbusplus::bus::match::rules::propertiesChangedNamespace(
            nsmAsyncBasePath, nsmAsyncStatusIntfName);
    {
        std::unique_lock<std::mutex> lock(mtx);
        statusMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus, matchRule,
            [this, &asyncObjectPath, &status](sdbusplus::message_t& msg) {
                if (msg.get_path() != asyncObjectPath)
                {
                    return;
                }
                std::string interface;
                std::map<std::string, std::variant<std::string>> properties;
                msg.read(interface, properties);
                auto it = properties.find("Status");
                if (it != properties.end())
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    status = std::get<std::string>(it->second);
                    status = status.substr(status.find_last_of('.') + 1);
                }
            });
    }
    asyncObjectPath = makeDebugTokenMethodCall(path, methodName, arg);
    if (asyncObjectPath.empty())
    {
        log<level::ERR>(
            (path + ": " + methodName + " failed to get async object path")
                .c_str());
        return "";
    }
    try
    {
        auto method = bus.new_method_call(nsmService, asyncObjectPath.c_str(),
                                          propertiesIntfName, "Get");
        method.append(nsmAsyncStatusIntfName, "Status");
        auto reply = bus.call(method);
        std::variant<std::string> dbusStatus;
        reply.read(dbusStatus);
        status = std::get<std::string>(dbusStatus);
        status = status.substr(status.find_last_of('.') + 1);
        if (status != "InProgress")
        {
            if (status != "Success")
            {
                logAsyncError(path, methodName, status);
                return "";
            }
            return asyncObjectPath;
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>((path + ": failed to get initial async status: " +
                         std::string(e.what()))
                            .c_str());
        return "";
    }
    auto maxIterations = std::chrono::seconds(propertyChangeSignalTimeout) /
                         std::chrono::milliseconds(100);
    for (auto i = 0; i < maxIterations; ++i)
    {
        bus.process_discard();
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (status != "InProgress")
            {
                break;
            }
        }
        bus.wait(std::chrono::milliseconds(100));
    }
    if (status != "Success")
    {
        logAsyncError(path, methodName, status);
        return "";
    }
    return asyncObjectPath;
}

std::string UpdateDebugToken::getTokenStatus(const std::string& path)
{
    try
    {
        auto asyncPath =
            handleAsyncCall(path, "GetStatus", std::string(nsmTokenTypeCRDT));
        if (asyncPath.empty())
        {
            return "";
        }
        auto statusValue = getAsyncValue(asyncPath);
        auto& [tokenType, tokenStatus, additionalInfo, timeLeft] =
            std::get<NSMTokenStatusTuple>(statusValue);
        return tokenStatus;
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            (path + ": failed to get token status: " + std::string(e.what()))
                .c_str());
        return "";
    }
}

int UpdateDebugToken::nsmTokenErase()
{
    int status = 0;
    NSMEndpoints nsmEndpoints;
    if (enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1;
    }
    if (nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No NSM debug token endpoints found.");
        return 0;
    }
    for (const auto& path : nsmEndpoints)
    {
        try
        {
            // Get initial status
            std::string tokenStatus = getTokenStatus(path);
            if (tokenStatus.empty())
            {
                continue;
            }
            if (tokenStatus != nsmTokenStatusDebugSessionActive &&
                tokenStatus != nsmTokenStatusTokenTimeout)
            {
                log<level::INFO>((path + ": no token installed").c_str());
                continue;
            }

            // Disable tokens
            auto asyncPath = handleAsyncCall(path, "DisableTokens");
            if (asyncPath.empty())
            {
                log<level::ERR>((path + ": DisableTokens failed").c_str());
                status = -1;
                continue;
            }

            // Verify token status after disable
            tokenStatus = getTokenStatus(path);
            if (tokenStatus.empty())
            {
                status = -1;
                continue;
            }
            if (tokenStatus != nsmTokenStatusNoTokenApplied)
            {
                log<level::ERR>((path + ": token erase failed").c_str());
                status = -1;
                continue;
            }
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(
                (path + ": NSM D-Bus Exception: " + std::string(e.what()))
                    .c_str());
            status = -1;
            continue;
        }
    }
    return status;
}

int UpdateDebugToken::nsmTokenInstall(TokenMap& tokens)
{
    int status = 0;
    Token token{};
    NSMEndpoints nsmEndpoints;
    if (enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1;
    }
    if (nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No NSM debug token endpoints found.");
        return 0;
    }
    for (const auto& path : nsmEndpoints)
    {
        try
        {
            // Get initial status
            std::string tokenStatus = getTokenStatus(path);
            if (tokenStatus.empty())
            {
                log<level::ERR>((path + ": empty token status.").c_str());
                continue;
            }
            if (tokenStatus == nsmTokenStatusDebugSessionActive)
            {
                log<level::INFO>((path + ": debug session active.").c_str());
                continue;
            }

            // Get device ID
            auto method = bus.new_method_call(nsmService, path.c_str(),
                                              propertiesIntfName, "Get");
            method.append(nsmDebugTokenIntfName, "TokenDeviceID");
            auto reply = bus.call(method);
            std::variant<std::string> property;
            reply.read(property);
            const std::string serialNumber = std::get<std::string>(property);
            if (tokens.find(serialNumber) != tokens.end())
            {
                // Strip 44 bytes from the header
                token = std::vector<uint8_t>(tokens[serialNumber].begin() + 44,
                                             tokens[serialNumber].end());
            }
            else
            {
                log<level::ERR>(
                    (path + ": no token for serial number: " + serialNumber)
                        .c_str());
                continue;
            }

            // Install token
            auto asyncPath = handleAsyncCall(path, "InstallToken", token);
            if (asyncPath.empty())
            {
                log<level::ERR>((path + ": InstallToken failed").c_str());
                status = -1;
                continue;
            }

            // Verify token status after install
            tokenStatus = getTokenStatus(path);
            if (tokenStatus.empty())
            {
                status = -1;
                continue;
            }
            if (tokenStatus != nsmTokenStatusDebugSessionActive)
            {
                log<level::ERR>(
                    (path + ": token install failed, status: " + tokenStatus)
                        .c_str());
                status = -1;
                continue;
            }
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(
                (path + ": NSM D-Bus Exception: " + std::string(e.what()))
                    .c_str());
            continue;
        }
    }
    return status;
}
