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

std::string MCTPDiscoveryResource::getMCTPObjectPath()
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
        if (!mctpEID)
        {
            continue;
        }
        if (*mctpEID == eid)
        {
            return objectPath;
        }
    }
    return {};
}

void MCTPDiscoveryResource::monitorMCTPEndpoint()
{
    mctpObjectPath = getMCTPObjectPath();
    if (!mctpObjectPath.empty())
    {
        wasEnumeratedOnce = true;
    }

    if (!endpointAddedMatch)
    {
        endpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            MatchRules::interfacesAdded(mctpObjMgrPath.data()) +
                MatchRules::sender(mctpService),
            [this](sdbusplus::message::message& msg) {
                try
                {
                    sdbusplus::object_path addedPath;
                    nvidia::software::updater::InterfaceMap interfaces;
                    msg.read(addedPath, interfaces);

                    if (!interfaces.contains(mctpEndpointIntfName))
                    {
                        return;
                    }

                    const auto* mctpEID = std::get_if<uint8_t>(
                        &interfaces.at(mctpEndpointIntfName).at("EID"));
                    if (mctpEID && *mctpEID == eid)
                    {
                        mctpObjectPath = addedPath.str;
                        wasEnumeratedOnce = true;
                        updateHealth();
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "Failed to process MCTP interfacesAdded signal: {ERROR}",
                        "ERROR", e);
                }
            });
    }

    if (!endpointRemovedMatch)
    {
        endpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            MatchRules::interfacesRemoved(mctpObjMgrPath.data()) +
                MatchRules::sender(mctpService),
            [this](sdbusplus::message::message& msg) {
                try
                {
                    sdbusplus::object_path removedPath;
                    msg.read(removedPath);

                    if (removedPath.str == mctpObjectPath)
                    {
                        mctpObjectPath.clear();
                        updateHealth();
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "Failed to process MCTP interfacesRemoved signal: {ERROR}",
                        "ERROR", e);
                }
            });
    }
}
