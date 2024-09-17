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

#include "ap_resource.hpp"

#include "dbusutils.hpp"
#include "erot_resource.hpp"
#include "handler.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>

using namespace phosphor::logging;

mctp_vdm::requester::Coroutine APResource::initializeHealth()
{
    if (isERoTHealthy())
    {
        co_await erotResource->updateBootStatusAsync();
    }

    if (isApHealthy())
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
        co_return 0;
    }

    if (isAPInRecovery())
    {
        timer =
            std::make_unique<sdbusplus::Timer>([&]() { updateHealthAsync(); });

        timer->start(std::chrono::seconds(maxBootCompleteTimeout), false);
    }
    else
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }

    co_return 0;
}

void APResource::startWatchingApEid() noexcept
{
    const auto objPath = std::string(mctpObjPathPrefix) + std::to_string(eid);
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    try
    {
        apMCTPService =
            dbusUtil.getServices(objPath.c_str(), mctpEndpointIntfName)[0];
    }
    catch (std::runtime_error& e)
    {
        lg2::error("D-Bus error while fetching service for {OBJECT}: {ERROR} ",
                   "OBJECT", objPath, "ERROR", e.what());
    }

    if (apMCTPService.empty())
    {

        mctpApObjManagerMatch.emplace_back(
            bus, MatchRules::interfacesAdded("/xyz/openbmc_project/mctp"),
            [&]([[maybe_unused]] sdbusplus::message::message& msg) {
                startWatchingApEid();
            });
        return;
    }

    mctpApObjManagerMatch.clear();

    deviceMatches.emplace_back(bus,
                               MatchRules::propertiesChanged(
                                   objPath.c_str(), mctpEndpointEnableIntfName),
                               std::bind(&APResource::onMCTPDiscoveryMsg, this,
                                         std::placeholders::_1));
    deviceMatches.emplace_back(bus,
                               MatchRules::interfacesAdded(objPath.c_str()),
                               std::bind(&APResource::onMCTPDiscoveryMsg, this,
                                         std::placeholders::_1));
}

mctp_vdm::requester::Coroutine APResource::updateHealthAsync()
{
    if (isERoTHealthy())
    {
        co_await erotResource->updateBootStatusAsync();
    }

    if (isApHealthy())
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
        co_return 0;
    }

    if (isAPInRecovery())
    {
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
    }
    else
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }

    co_return 0;
}

bool APResource::isERoTHealthy() const noexcept
{
    return erotResource->isDeviceEnumerated() and
           erotResource->checkForEnabledMCTPEids();
}

bool APResource::isAPInRecovery() const noexcept
{
    if (isERoTHealthy())
    {
        auto status = erotResource->getBootStatus();

        auto bootCompleteTimeout = (status[status.size() - 4]) & (1 << 3);

        return static_cast<bool>(bootCompleteTimeout);
    }
    return !isApHealthy();
}
