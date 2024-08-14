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
#include <boost/container/flat_map.hpp>
#include <filesystem>
#include <iostream>
#include <thread>
#include <future>
#include <chrono>

/*
    Function to get operation status from the dbus signal message.
    MESSAGE "sa{sv}as" {
            STRING "xyz.openbmc_project.Common.Progress";
            ARRAY "{sv}" {
                    DICT_ENTRY "sv" {
                            STRING "Status";
                            VARIANT "s" {
                                    STRING "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
                            };
                    };
            };
            ARRAY "s" {
            };
    };
*/
std::string getOperationStatus(sdbusplus::message_t &msg)
{
    std::string interfaceName{};
    std::string response;
    std::map<std::string, std::variant<std::string>>  properties;       
    std::tuple<uint8_t, uint8_t, uint8_t, uint32_t> tknStatus;
    try
    {
        msg.read(interfaceName, properties);
    }
    catch(const std::exception& e)
    {
        log<level::ERR>("D-Bus error", 
            entry("ERROR=%s", e.what()));
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

int UpdateDebugToken::progressStatusPropertyChange(sdbusplus::message_t &msg){
    nsmOperationStatus = getOperationStatus(msg);;
    cv.notify_one();
    return 0;
}


int UpdateDebugToken::enumerateNsmDebugTokenEndpoints(NSMEndpoints &nsmEndpoints)
{
    dbus::GetSubTreeResponse objects{};
    const dbus::Interfaces ifaceList{nsmDebugTokenIntfName};
    try
    {
        auto method = bus.new_method_call(objectMapperService, objectMapperPath,
                                          objectMapperIntfName,
                                          "GetSubTree");
        method.append(nsmDebugTokenPath, 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(objects);
        for ([[maybe_unused]] const auto& [objectPath, mapperServiceMap] : objects){
            nsmEndpoints.push_back(objectPath);
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(e.what());
        return -1;
    }
    return 0;
}

/**
*    Token Status is value of type (yyyu).
*    .TokenStatus                                property  (yyyu)    2 5 6 2098858688                         emits-change writable
*
*    The four values are: Status, AdditionalInfo, TokenType and Time left.
*    TokenType should be CRDT (0x06).
*
*    @return int - Status value from TokenStatus
*/

int getTokenStatus(sdbusplus::bus::bus& bus, std::string path)
{
    std::variant<std::tuple<uint8_t, uint8_t, uint8_t, uint32_t>> 
                    tokenStatusProperty;
    std::tuple<uint8_t, uint8_t, uint8_t, uint32_t> tknStatus;
    auto method = bus.new_method_call(nsmService, path.c_str(),
                                          propertiesPath,
                                          "Get");
    method.append(nsmDebugTokenIntfName, "TokenStatus");
    auto reply = bus.call(method);
    reply.read(tokenStatusProperty);            
    tknStatus = 
        std::get<std::tuple<uint8_t, uint8_t, uint8_t, uint32_t>>(
                tokenStatusProperty);
    uint8_t tokenType = std::get<2>(tknStatus);
    if(tokenType != nsmTokenTypeCRDT)
    {
        log<level::ERR>("Invalid token type.");
        return -1;
    }
    return std::get<0>(tknStatus);
}

/**
 * Worker function to process dbus messages while the main thread is waiting.
 * This would run as a seperate thread.
 * 
 * It is terminated when the promise value is set by the main thread.
*/
void worker(std::future<void> futureObj, sdbusplus::bus::bus& bus) {
    while (futureObj.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout) {
        bus.process_discard();
    }
}

int UpdateDebugToken::nsmTokenErase()
{
    int status = 0;
    std::string response;
    NSMEndpoints nsmEndpoints;
    std::mutex mtx;
    std::vector<sdbusplus::bus::match_t> propertyChangeCallbacks;
    if(enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1; 
    }
    if(nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No Nsm debug token endpoints found.");
        return 0; 
    }

    // Worker thread to process dbus signals
    std::promise<void> exitSignal;
    std::future<void> futureObj = exitSignal.get_future();
    std::thread thr(worker, std::move(futureObj), std::ref(bus));

    for(const auto& path: nsmEndpoints)
    {
        std::unique_lock<std::mutex> lock(mtx);
        uint8_t tokenStatus;

        std::string propertiesMatchString = 
            sdbusplus::bus::match::rules::propertiesChanged(
                path, nsmProgressIntfName);
        sdbusplus::bus::match_t match(
            bus, propertiesMatchString,
            [&](sdbusplus::message::message& msg){
                this->progressStatusPropertyChange(msg);
            }
        );

        auto method = bus.new_method_call(nsmService, path.c_str(),
                                nsmDebugTokenIntfName,
                                "GetStatus");
        method.append(nsmTokenTypeCRDT);
        bus.call(method);
        // Wait for GetStatus operation to complete
        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout)) 
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for GetStatus command");
            status = -1;
            createTokenEraseErrorMessage(path);
            continue;
        }
        tokenStatus = getTokenStatus(bus, path); 


        if(tokenStatus != static_cast<int>(NSMTokenStatus::DebugSessionActive) &&
            tokenStatus != static_cast<int>(NSMTokenStatus::TokenTimeout))
        {
            log<level::INFO>("No token installed.");
            continue;
        }

        method = bus.new_method_call(nsmService, path.c_str(),
                                        nsmDebugTokenIntfName,
                                        "DisableTokens");
        bus.call(method);
        // Wait for DisableTokens operation to complete
        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout)) 
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for DisableTokens command");
            status = -1;
            createTokenEraseErrorMessage(path);
            continue;
        }

        method = bus.new_method_call(nsmService, path.c_str(),
                                nsmDebugTokenIntfName,
                                "GetStatus");
        method.append(nsmTokenTypeCRDT);
        bus.call(method);
        // Wait for GetStatus operation to complete
        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout))
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for GetStatus command");
            status = -1;
            createTokenEraseErrorMessage(path);
            continue;
        }

        tokenStatus = getTokenStatus(bus, path); 
        if(tokenStatus != static_cast<int>(NSMTokenStatus::NoTokenApplied))
        {
            log<level::ERR>(("Token erase failed for: {}" + path).c_str());
            status = -1;
            createTokenEraseErrorMessage(path);
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
    std::vector<sdbusplus::bus::match_t> propertyChangeCallbacks;
    if(enumerateNsmDebugTokenEndpoints(nsmEndpoints) != 0)
    {
        log<level::ERR>("NSM Endpoints enumeration error");
        return -1; 
    }
    if(nsmEndpoints.size() == 0)
    {
        log<level::INFO>("No Nsm debug token endpoints found.");
        return 0; 
    }

    // Worker thread to process dbus signals
    std::promise<void> exitSignal;
    std::future<void> futureObj = exitSignal.get_future();
    std::thread thr(worker, std::move(futureObj), std::ref(bus));

    for(const auto& path: nsmEndpoints)
    {
        std::unique_lock<std::mutex> lock(mtx);
        std::variant<std::string> property;
        uint8_t tokenStatus;
        std::string propertiesMatchString = 
            sdbusplus::bus::match::rules::propertiesChanged(
                path, nsmProgressIntfName);

        sdbusplus::bus::match_t match(
            bus, propertiesMatchString,
            [&](sdbusplus::message::message& msg){
                this->progressStatusPropertyChange(msg);
            }
        );

        auto method = bus.new_method_call(nsmService, path.c_str(),
                                nsmDebugTokenIntfName,
                                "GetStatus");
        method.append(nsmTokenTypeCRDT);
        bus.call(method);

        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout)) 
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for GetStatus command");
            status = -1;
            createTokenEraseErrorMessage(path);
            continue;
        }

        tokenStatus = getTokenStatus(bus, path); 
        if(tokenStatus == static_cast<int>(NSMTokenStatus::DebugSessionActive))
        {
            log<level::INFO>("Debug session active.");
            continue;
        }

        method = bus.new_method_call(nsmService, path.c_str(),
                                          propertiesPath,
                                          "Get");
        method.append(nsmDebugTokenIntfName, "TokenDeviceID");
        auto reply = bus.call(method);
        reply.read(property);

        const std::string serialNumber = std::get<std::string>(property) ;
        if(tokens.find(serialNumber) != tokens.end())
        {
            // Strip 44 bytes from the header
            token = std::vector<uint8_t>(tokens[serialNumber].begin() + 44, 
                                        tokens[serialNumber].end());;
        }
        else
        {
            log<level::ERR>(("No token for serial number:" + serialNumber).c_str() );
            continue;
        }

        method = bus.new_method_call(nsmService, path.c_str(),
                                        nsmDebugTokenIntfName,
                                        "InstallToken");
        method.append(token);
        reply = bus.call(method);

        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout)) 
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for InstallToken command");
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }

        method = bus.new_method_call(nsmService, path.c_str(),
                                nsmDebugTokenIntfName,
                                "GetStatus");
        method.append(nsmTokenTypeCRDT);
        bus.call(method);

        if(cv.wait_for(lock, std::chrono::seconds(propertyChangeSignalTimeout)) 
                    == std::cv_status::timeout)
        {
            log<level::ERR>("Timeout waiting for GetStatus command");
            status = -1;
            createTokenInstallErrorMessage(path);
            continue;
        }

        tokenStatus = getTokenStatus(bus, path); 
        if(tokenStatus != static_cast<int>(NSMTokenStatus::DebugSessionActive))
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