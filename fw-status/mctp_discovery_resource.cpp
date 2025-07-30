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

#include "mctp_discovery_resource.hpp"

#include "dbusutils.hpp"

std::unordered_set<std::string>
    MCTPDiscoveryResource::getMctpServices() const noexcept
{
    nvidia::software::updater::GetSubTreeResponse getSubTreeResponse{};
    std::unordered_set<std::string> mctpCtrlServices{};
    const nvidia::software::updater::Interfaces ifaceList{mctpEndpointIntfName};
    try
    {
        auto method = bus.new_method_call(mapperService, mapperPath,
                                          mapperInterface, "GetSubTree");
        method.append(mctpObjPathPrefix.data(), 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "D-Bus error calling Subtrees method on ObjectMapper: {ERROR}",
            "ERROR", e.what());
    }

    for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
    {
        for (const auto& [service, interfaces] : mapperServiceMap)
        {
            mctpCtrlServices.insert(service);
        }
    }

    return mctpCtrlServices;
}

std::unordered_map<std::string, std::string>
    MCTPDiscoveryResource::getMCTPObjects()
{
    std::unordered_map<std::string, std::string> mctpObjects{};
    const auto& mctpCtrlServices = getMctpServices();
    for (const auto& serviceName : mctpCtrlServices)
    {
        auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
        const auto objects = dbusUtil.getManagedObjects(serviceName.c_str(),
                                                        mctpObjMgrPath.data());

        for (const auto& [objectPath, interfaces] : objects)
        {
            if (!interfaces.contains(uuidIntfName))
            {
                continue;
            }
            const auto& mctpUUID =
                std::get<std::string>(interfaces.at(uuidIntfName).at("UUID"));
            if (mctpUUID.c_str() != uuid)
            {
                continue;
            }
            mctpObjects[serviceName] = objectPath;
            break;
        }
    }
    return mctpObjects;
}

void MCTPDiscoveryResource::startWatchingMCTPObjects(bool needUpdateHealth)
{
    mctpEidObjects = getMCTPObjects();
    if (mctpEidObjects.empty())
    {
        mctpObjManagerMatch.emplace_back(
            bus, MatchRules::interfacesAdded(mctpObjMgrPath.data()),
            [&]([[maybe_unused]] sdbusplus::message::message& msg) {
                startWatchingMCTPObjects(true);
            });
        return;
    }

    mctpObjManagerMatch.clear();

    for (const auto& [service, mctpObject] : mctpEidObjects)
    {
        deviceMatches.emplace_back(
            bus,
            MatchRules::propertiesChanged(mctpObject.c_str(),
                                          mctpEndpointEnableIntfName),
            std::bind(&MCTPDiscoveryResource::onMCTPDiscoveryMsg, this,
                      std::placeholders::_1));

        deviceMatches.emplace_back(
            bus, MatchRules::interfacesAdded(mctpObject.c_str()),
            std::bind(&MCTPDiscoveryResource::onMCTPDiscoveryMsg, this,
                      std::placeholders::_1));
    }

    if (needUpdateHealth)
    {
        // Force a health status update since we might have missed the signals
        // during MCTP enumeration. The signals
        // (propertiesChanged/interfacesAdded) could have been sent before we
        // set up the matches above.
        updateHealth();
    }
}

bool MCTPDiscoveryResource::checkForEnabledMCTPEids() const noexcept
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    std::string ret{};
    for (const auto& [service, mctpObject] : mctpEidObjects)
    {
        try
        {
            ret = dbusUtil.getProperty<std::string>(
                service.c_str(), mctpObject.c_str(), mctpEndpointEnableIntfName,
                "Connectivity");
            // return true if any of the MCTP EIDs are enabled
            if (ret == "Available")
            {
                return true;
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to get Connectivity property for {OBJECT} on service {SERVICE}. Error: {ERROR}",
                "OBJECT", mctpObject, "SERVICE", service, "ERROR", e.what());
        }
    }
    return ret == "Available";
}
