/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION &
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

#include "cpld_resource.hpp"

#include "dbusutils.hpp"
#include "mctp_discovery_resource.hpp"

#include <map>
#include <tuple>
#include <variant>
#include <vector>

namespace
{

constexpr auto loggingService = "xyz.openbmc_project.Logging";
constexpr auto loggingObjPath = "/xyz/openbmc_project/logging";
constexpr auto loggingEntryIntf = "xyz.openbmc_project.Logging.Entry";
constexpr auto propertiesIntf = "org.freedesktop.DBus.Properties";
constexpr auto mapperBusName = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr auto cpldAuthFailErrorID = "HPM-CPLD-AUTH-FAIL";
constexpr auto errorIDProperty = "ERROR_ID";
constexpr auto deviceNameProperty = "DEVICE_NAME";
constexpr auto additionalDataProperty = "AdditionalData";

using LoggingAdditionalData = std::map<std::string, std::string>;
using LoggingValue = std::variant<
    bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t, uint64_t,
    double, std::string, std::vector<uint8_t>, LoggingAdditionalData,
    std::vector<std::tuple<std::string, std::string, std::string>>>;
using LoggingPropertyMap = std::map<std::string, LoggingValue>;
using LoggingInterfaceMap = std::map<std::string, LoggingPropertyMap>;

bool hasLoggingEntryPrefix(const std::string& objectPath)
{
    static const std::string entryPrefix =
        std::string(loggingObjPath) + "/entry/";
    return objectPath.rfind(entryPrefix, 0) == 0;
}

} // namespace

namespace MatchRules = sdbusplus::bus::match::rules;

CpldResource::CpldResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                           uint8_t smaEid, const std::string& deviceId) :
    BaseResource(bus, objPath), smaEid(smaEid), deviceId(deviceId)
{
    monitorLoggingEvents();
    checkExistingAuthFail();
    monitorSMAEndpoint();
    updateHealth();
}

std::string CpldResource::getSMAMCTPObjectPath()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    const auto objects =
        dbusUtil.getManagedObjects(mctpService, mctpObjMgrPath.data());

    for (const auto& [objectPath, interfaces] : objects)
    {
        if (!interfaces.contains(mctpEndpointIntfName))
        {
            continue;
        }

        const auto* mctpEID = std::get_if<uint8_t>(
            &interfaces.at(mctpEndpointIntfName).at("EID"));

        if (mctpEID && (*mctpEID == smaEid))
        {
            return objectPath;
        }
    }
    return {};
}

void CpldResource::monitorSMAEndpoint()
{
    smaEndpointObjectPath = getSMAMCTPObjectPath();
    if (!smaEndpointObjectPath.empty())
    {
        hasObservedSMAEndpoint = true;
    }

    smaEndpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        MatchRules::interfacesAdded(mctpObjMgrPath.data()) +
            MatchRules::sender(mctpService),
        [this](sdbusplus::message::message& msg) {
            try
            {
                sdbusplus::message::object_path addedPath;
                nvidia::software::updater::InterfaceMap interfaces;
                msg.read(addedPath, interfaces);

                if (!interfaces.contains(mctpEndpointIntfName))
                {
                    return;
                }

                const auto* mctpEID = std::get_if<uint8_t>(
                    &interfaces.at(mctpEndpointIntfName).at("EID"));
                if (mctpEID && (*mctpEID == smaEid))
                {
                    smaEndpointObjectPath = addedPath.str;
                    hasObservedSMAEndpoint = true;
                    updateHealth();
                }
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "CpldResource: failed to process SMA InterfacesAdded: {ERROR}",
                    "ERROR", e);
            }
        });

    smaEndpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        MatchRules::interfacesRemoved(mctpObjMgrPath.data()) +
            MatchRules::sender(mctpService),
        [this](sdbusplus::message::message& msg) {
            try
            {
                sdbusplus::message::object_path removedPath;
                msg.read(removedPath);

                if (removedPath.str == smaEndpointObjectPath)
                {
                    smaEndpointObjectPath.clear();
                    updateHealth();
                }
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "CpldResource: failed to process SMA InterfacesRemoved: {ERROR}",
                    "ERROR", e);
            }
        });
}

void CpldResource::handleLogEntry(
    const std::map<std::string, std::string>& additionalData)
{
    const auto errorIDIt = additionalData.find(errorIDProperty);
    const auto deviceNameIt = additionalData.find(deviceNameProperty);

    if (errorIDIt == additionalData.end() ||
        deviceNameIt == additionalData.end())
    {
        return;
    }

    if (errorIDIt->second == cpldAuthFailErrorID &&
        deviceNameIt->second == deviceId)
    {
        lg2::info("CpldResource: {DEV} auth-fail log entry matched", "DEV",
                  deviceId);
        authFailed = true;
    }
}

bool CpldResource::processLogEntry(const std::string& logEntryPath)
{
    try
    {
        auto method = bus.new_method_call(loggingService, logEntryPath.c_str(),
                                          propertiesIntf, "Get");
        method.append(loggingEntryIntf, "AdditionalData");

        auto reply = bus.call(method);
        std::variant<std::map<std::string, std::string>> propertyValue;
        reply.read(propertyValue);

        handleLogEntry(
            std::get<std::map<std::string, std::string>>(propertyValue));
        return true;
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "CpldResource: failed to process log entry at {PATH}: {ERROR}",
            "PATH", logEntryPath, "ERROR", e);
        return false;
    }
}

void CpldResource::checkExistingAuthFail()
{
    lg2::info("CpldResource: scanning existing log entries for {DEV}", "DEV",
              deviceId);

    try
    {
        auto method = bus.new_method_call(mapperBusName, mapperPath,
                                          mapperInterface, "GetSubTreePaths");
        method.append(std::string(loggingObjPath));
        method.append(0);
        method.append(std::vector<std::string>({loggingEntryIntf}));

        auto reply = bus.call(method);
        std::vector<std::string> logEntryPaths;
        reply.read(logEntryPaths);

        for (const auto& logEntryPath : logEntryPaths)
        {
            if (authFailed)
            {
                break;
            }
            processLogEntry(logEntryPath);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("CpldResource: failed to scan existing log entries: {ERROR}",
                   "ERROR", e);
    }
}

void CpldResource::monitorLoggingEvents()
{
    lg2::info("CpldResource: subscribing to logging events for {DEV}", "DEV",
              deviceId);

    logEntryAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        MatchRules::interfacesAdded(loggingObjPath) +
            MatchRules::sender(loggingService),
        [this](sdbusplus::message::message& msg) {
            try
            {
                sdbusplus::message::object_path addedPath;
                LoggingInterfaceMap interfaces;
                msg.read(addedPath, interfaces);

                if (!hasLoggingEntryPrefix(addedPath.str))
                {
                    return;
                }

                const auto interfaceIt = interfaces.find(loggingEntryIntf);
                if (interfaceIt == interfaces.end())
                {
                    return;
                }

                const auto additionalDataIt =
                    interfaceIt->second.find(additionalDataProperty);
                if (additionalDataIt == interfaceIt->second.end())
                {
                    return;
                }

                const auto* additionalData = std::get_if<LoggingAdditionalData>(
                    &additionalDataIt->second);
                if (!additionalData)
                {
                    lg2::error(
                        "CpldResource: AdditionalData has unexpected type for {PATH}",
                        "PATH", addedPath.str);
                    return;
                }

                handleLogEntry(*additionalData);
                updateHealth();
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "CpldResource: failed to process logging InterfacesAdded: {ERROR}",
                    "ERROR", e);
            }
        });
}

void CpldResource::updateHealth()
{
    if (authFailed)
    {
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        return;
    }

    if (!smaEndpointObjectPath.empty())
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }
    else if (hasObservedSMAEndpoint)
    {
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::UnavailableOffline);
    }
    else
    {
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::Absent);
    }
}
