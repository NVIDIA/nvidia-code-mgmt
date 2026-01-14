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

void MCTPDiscoveryResource::startWatchingMCTPObjects(bool needUpdateHealth)
{
    mctpObjectPath = getMCTPObjectPath();
    if (mctpObjectPath.empty())
    {
        if (!mctpObjManagerMatch)
        {
            mctpObjManagerMatch = std::make_unique<sdbusplus::bus::match_t>(
                bus, MatchRules::interfacesAdded(mctpObjMgrPath.data()),
                [&]([[maybe_unused]] sdbusplus::message::message& msg) {
                    startWatchingMCTPObjects(true);
                });
        }
        return;
    }

    mctpObjManagerMatch.reset();

    deviceMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        MatchRules::propertiesChanged(mctpObjectPath.c_str(),
                                      mctpEndpointEnableIntfName),
        std::bind(&MCTPDiscoveryResource::onMCTPDiscoveryMsg, this,
                  std::placeholders::_1));

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
    if (mctpObjectPath.empty())
    {
        return false;
    }

    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    try
    {
        auto ret = dbusUtil.getProperty<std::string>(
            mctpService, mctpObjectPath.c_str(), mctpEndpointEnableIntfName,
            "Connectivity");
        return ret == "Available";
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to get Connectivity property for EID {EID} at {OBJECT}. Error: {ERROR}",
            "EID", eid, "OBJECT", mctpObjectPath, "ERROR", e.what());
    }
    return false;
}
