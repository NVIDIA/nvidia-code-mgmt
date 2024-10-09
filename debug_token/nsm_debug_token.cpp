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
#include <future>
#include <iostream>
#include <thread>

/*
    Function to get operation status from the dbus signal message.
    MESSAGE "sa{sv}as" {
            STRING "xyz.openbmc_project.Common.Progress";
            ARRAY "{sv}" {
                    DICT_ENTRY "sv" {
                            STRING "Status";
                            VARIANT "s" {
                                    STRING
   "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
                            };
                    };
            };
            ARRAY "s" {
            };
    };
*/
std::string getOperationStatus(sdbusplus::message_t& msg)
{
    std::string interfaceName{};
    std::string response;
    std::map<std::string, std::variant<std::string>> properties;
    try
    {
        msg.read(interfaceName, properties);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("D-Bus error", entry("ERROR=%s", e.what()));
        return "";
    }

    auto it = properties.find("Status");
    if (it != properties.end())
    {
        const std::string statusData = std::get<std::string>(it->second);
        return statusData;
    }
    else
    {
        log<level::ERR>("Status property not found");
        return "";
    }
    return "";
}

int UpdateDebugToken::progressStatusPropertyChange(sdbusplus::message_t& msg)
{
    nsmOperationStatus = getOperationStatus(msg);
    cv.notify_one();
    return 0;
}

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

int getErrorCode(sdbusplus::bus::bus& bus, std::string path)
{
    std::variant<std::tuple<uint16_t, std::string>> errorCodeProperty;
    std::tuple<uint16_t, std::string> errorCode;
    try
    {
        auto method = bus.new_method_call(nsmService, path.c_str(),
                                          propertiesPath, "Get");
        method.append(nsmDebugTokenIntfName, "ErrorCode");
        auto reply = bus.call(method);
        reply.read(errorCodeProperty);
        errorCode =
            std::get<std::tuple<uint16_t, std::string>>(errorCodeProperty);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            ("NSM Command Exception DBUS_ERROR: " + std::string(e.what()))
                .c_str());
        return -1;
    }
    return std::get<0>(errorCode);
}

std::string getTokenStatus(sdbusplus::bus::bus& bus, std::string path)
{
    std::variant<std::tuple<std::string, std::string, std::string, uint32_t>>
        tokenStatusProperty;
    std::tuple<std::string, std::string, std::string, uint32_t> tknStatus;
    try
    {
        auto method = bus.new_method_call(nsmService, path.c_str(),
                                          propertiesPath, "Get");
        method.append(nsmDebugTokenIntfName, "TokenStatus");
        auto reply = bus.call(method);
        reply.read(tokenStatusProperty);
        tknStatus = std::get<
            std::tuple<std::string, std::string, std::string, uint32_t>>(
            tokenStatusProperty);
        std::string tokenType = std::get<0>(tknStatus);
        if (tokenType != nsmTokenTypeCRDT)
        {
            log<level::ERR>("Invalid token type.");
            return "";
        }
        return std::get<1>(tknStatus);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            ("NSM Command Exception DBUS_ERROR: " + std::string(e.what()))
                .c_str());
        return "";
    }
}

/**
 * Worker function to process dbus messages while the main thread is waiting.
 * This would run as a seperate thread.
 *
 * It is terminated when the promise value is set by the main thread.
 */
void worker(std::future<void> futureObj, sdbusplus::bus::bus& nsmBusIn,
            std::mutex& mtx)
{
    while (futureObj.wait_for(std::chrono::milliseconds(100)) ==
           std::future_status::timeout)
    {
        {
            std::unique_lock<std::mutex> lock(mtx);
            nsmBusIn.process_discard();
        }
    }
}

int UpdateDebugToken::nsmTokenErase()
{
    int status = 0;
    std::string response;
    NSMEndpoints nsmEndpoints;
    std::mutex mtx;
    std::unique_ptr<sdbusplus::bus::match_t> getStatusMatch;
    if (enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1;
    }
    if (nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No Nsm debug token endpoints found.");
        return 0;
    }

    // Worker thread to process dbus signals
    std::promise<void> exitSignal;
    std::future<void> futureObj = exitSignal.get_future();
    auto nsmBusThread = sdbusplus::bus::new_bus();
    std::thread thr(worker, std::move(futureObj), std::ref(nsmBusThread),
                    std::ref(mtx));

    for (const auto& path : nsmEndpoints)
    {
        std::string tokenStatus;

        std::string propertiesMatchString =
            sdbusplus::bus::match::rules::propertiesChanged(
                path, nsmProgressIntfName);
        try
        {
            {
                std::unique_lock<std::mutex> lock(mtx);
                getStatusMatch.reset();
                getStatusMatch = std::make_unique<sdbusplus::bus::match_t>(
                    nsmBusThread, propertiesMatchString,
                    [this](sdbusplus::message::message& msg) {
                        this->progressStatusPropertyChange(msg);
                    });
            }
            auto method = bus.new_method_call(
                nsmService, path.c_str(), nsmDebugTokenIntfName, "GetStatus");
            method.append(nsmTokenTypeCRDT);
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception Erase.GetStatus: " +
                             std::string(e.what()))
                                .c_str());
            status = -1;
            sdbusplus::message::object_path devicePath(path);
            std::string deviceName = devicePath.filename();
            createMessageRegistryResourceErrors(
                debugTokenEraseFailed, deviceName, OperationType::Common,
                static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                "GetStatus");
            continue;
        }
        // Wait for GetStatus operation to complete
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for GetStatus command for path: " + path)
                        .c_str());
                status = -1;
                sdbusplus::message::object_path devicePath(path);
                std::string deviceName = devicePath.filename();
                createMessageRegistryResourceErrors(
                    debugTokenEraseFailed, deviceName, OperationType::Common,
                    static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                    "GetStatus");
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            if (getErrorCode(bus, path) == nsmUnsupportedCmd)
            {
                log<level::INFO>(
                    ("Debug token operation not supported for " + path)
                        .c_str());
            }
            else
            {
                log<level::ERR>("The operation didn't complete");
                status = -1;
            }
            continue;
        }

        tokenStatus = getTokenStatus(bus, path);
        if (tokenStatus != nsmTokenStatusDebugSessionActive &&
            tokenStatus != nsmTokenStatusTokenTimeout)
        {
            log<level::INFO>("No token installed.");
            continue;
        }
        try
        {
            auto method =
                bus.new_method_call(nsmService, path.c_str(),
                                    nsmDebugTokenIntfName, "DisableTokens");
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception DisableTokens.GetStatus: " +
                             std::string(e.what()))
                                .c_str());

            status = -1;
            sdbusplus::message::object_path devicePath(path);
            std::string deviceName = devicePath.filename();
            createMessageRegistryResourceErrors(
                debugTokenEraseFailed, deviceName, OperationType::Common,
                static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                "DisableTokens");
            continue;
        }
        // Wait for DisableTokens operation to complete
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for DisableTokens command for path: " +
                     path)
                        .c_str());
                status = -1;
                sdbusplus::message::object_path devicePath(path);
                std::string deviceName = devicePath.filename();
                createMessageRegistryResourceErrors(
                    debugTokenEraseFailed, deviceName, OperationType::Common,
                    static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                    "DisableTokens");
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            log<level::ERR>("The operation didn't complete");
            status = -1;
            continue;
        }

        try
        {
            auto method = bus.new_method_call(
                nsmService, path.c_str(), nsmDebugTokenIntfName, "GetStatus");
            method.append(nsmTokenTypeCRDT);
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception EraseStatus.GetStatus:  " +
                             std::string(e.what()))
                                .c_str());
            status = -1;
            createTokenEraseErrorMessage(path);
            continue;
        }
        // Wait for GetStatus operation to complete
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for GetStatus command for path: " + path)
                        .c_str());
                status = -1;
                sdbusplus::message::object_path devicePath(path);
                std::string deviceName = devicePath.filename();
                createMessageRegistryResourceErrors(
                    debugTokenEraseFailed, deviceName, OperationType::Common,
                    static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                    "GetStatus");
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            log<level::ERR>("The operation didn't complete");
            status = -1;
            continue;
        }

        tokenStatus = getTokenStatus(bus, path);
        if (tokenStatus != nsmTokenStatusNoTokenApplied)
        {
            log<level::ERR>(("Token erase failed for: {}" + path).c_str());
            status = -1;
            sdbusplus::message::object_path devicePath(path);
            std::string deviceName = devicePath.filename();
            createMessageRegistryResourceErrors(
                debugTokenEraseFailed, deviceName, OperationType::Common,
                static_cast<int>(CommonErrorCodes::NSMCommandFailure),
                "DisableTokens");
            continue;
        }
    }
    // Send exit signal to thread and wait for it to terminate.
    exitSignal.set_value();
    thr.join();
    return status;
}

int UpdateDebugToken::nsmTokenInstall(TokenMap& tokens)
{
    int status = 0;
    std::string response;
    Token token{};
    NSMEndpoints nsmEndpoints;
    std::mutex mtx;
    std::unique_ptr<sdbusplus::bus::match_t> getStatusMatch;
    if (enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1;
    }
    if (nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No Nsm debug token endpoints found.");
        return 0;
    }

    // Worker thread to process dbus signals
    std::promise<void> exitSignal;
    std::future<void> futureObj = exitSignal.get_future();
    auto nsmBusThread = sdbusplus::bus::new_bus();
    std::thread thr(worker, std::move(futureObj), std::ref(nsmBusThread),
                    std::ref(mtx));

    for (const auto& path : nsmEndpoints)
    {
        std::variant<std::string> property;
        std::string tokenStatus;
        std::string propertiesMatchString =
            sdbusplus::bus::match::rules::propertiesChanged(
                path, nsmProgressIntfName);
        try
        {
            {
                std::unique_lock<std::mutex> lock(mtx);
                getStatusMatch.reset();
                getStatusMatch = std::make_unique<sdbusplus::bus::match_t>(
                    nsmBusThread, propertiesMatchString,
                    [this](sdbusplus::message::message& msg) {
                        this->progressStatusPropertyChange(msg);
                    });
            }
            auto method = bus.new_method_call(
                nsmService, path.c_str(), nsmDebugTokenIntfName, "GetStatus");
            method.append(nsmTokenTypeCRDT);
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception Install.GetStatus: " +
                             std::string(e.what()))
                                .c_str());
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for GetStatus command for path: " + path)
                        .c_str());
                status = -1;
                createTokenInstallErrorMessage(path);
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            if (getErrorCode(bus, path) == nsmUnsupportedCmd)
            {
                log<level::INFO>(
                    ("Debug token operation not supported for " + path)
                        .c_str());
            }
            else
            {
                log<level::ERR>("The operation didn't complete");
                status = -1;
            }
            continue;
        }

        tokenStatus = getTokenStatus(bus, path);
        if (tokenStatus == nsmTokenStatusDebugSessionActive)
        {
            log<level::INFO>("Debug session active.");
            continue;
        }
        try
        {
            auto method = bus.new_method_call(nsmService, path.c_str(),
                                              propertiesPath, "Get");
            method.append(nsmDebugTokenIntfName, "TokenDeviceID");
            auto reply = bus.call(method);
            reply.read(property);

            const std::string serialNumber = std::get<std::string>(property);
            if (tokens.find(serialNumber) != tokens.end())
            {
                // Strip 44 bytes from the header
                token = std::vector<uint8_t>(tokens[serialNumber].begin() + 44,
                                             tokens[serialNumber].end());
                ;
            }
            else
            {
                log<level::ERR>(
                    ("No token for serial number:" + serialNumber).c_str());
                continue;
            }
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception Install.TokenId: " +
                             std::string(e.what()))
                                .c_str());
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }

        try
        {
            auto method =
                bus.new_method_call(nsmService, path.c_str(),
                                    nsmDebugTokenIntfName, "InstallToken");
            method.append(token);
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(
                ("NSM D-Bus Exception InstallToken: " + std::string(e.what()))
                    .c_str());
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for InstallToken command for path: " +
                     path)
                        .c_str());
                status = -1;
                createTokenInstallErrorMessage(path);
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            log<level::ERR>("The operation didn't complete");
            status = -1;
            continue;
        }

        try
        {
            auto method = bus.new_method_call(
                nsmService, path.c_str(), nsmDebugTokenIntfName, "GetStatus");
            method.append(nsmTokenTypeCRDT);
            bus.call(method);
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(("NSM D-Bus Exception InstallToken.GetStatus: " +
                             std::string(e.what()))
                                .c_str());
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(
                    lock, std::chrono::seconds(propertyChangeSignalTimeout)) ==
                std::cv_status::timeout)
            {
                log<level::ERR>(
                    ("Timeout waiting for GetStatus command for path: " + path)
                        .c_str());
                status = -1;
                createTokenInstallErrorMessage(path);
                continue;
            }
        }
        if (nsmOperationStatus != nsmCompletedStatus)
        {
            log<level::ERR>("The operation didn't complete");
            status = -1;
            continue;
        }

        tokenStatus = getTokenStatus(bus, path);
        if (tokenStatus != nsmTokenStatusDebugSessionActive)
        {
            log<level::ERR>(("Token install failed for: " + path).c_str());
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }
    }
    // Send exit signal to thread and wait for it to terminate.
    exitSignal.set_value();
    thr.join();
    return status;
}