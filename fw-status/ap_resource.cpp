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

#include "ap_resource.hpp"

#include "erot_resource.hpp"
#include "handler.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>

using namespace phosphor::logging;

template <typename T>
void APResource<T>::populateService(const std::string& objPath) noexcept
{
    try
    {
        auto mapper = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                          MAPPER_INTERFACE, "GetObject");

        mapper.append(objPath.c_str(),
                      std::vector<std::string>({mctpEndpointEnableIntfName}));
        auto mapperResponseMsg = bus.call(mapper);
        std::vector<std::pair<std::string, std::vector<std::string>>>
            mapperResponse;
        mapperResponseMsg.read(mapperResponse);

        apMCTPService = mapperResponse.at(0).first;
    }
    catch (const sdbusplus::exception::SdBusError& ex)
    {
        lg2::error(
            "Could not find {PATH}. MCTP EID for AP is not enumerated now",
            "PATH", objPath, "INTERFACE", mctpEndpointEnableIntfName);
    }
}

template <typename T>
mctp_vdm::requester::Coroutine APResource<T>::initializeHealth()
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
        timer = std::make_unique<sdbusplus::Timer>(
            [&]() { updateHealthAsync().detach(); });

        timer->start(std::chrono::seconds(maxBootCompleteTimeout), false);
    }
    else
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }

    co_return 0;
}

template <typename T>
void APResource<T>::startWatchingApEid() noexcept
{
    const auto objPath = std::string(mctpObjPathPrefix) + std::to_string(eid);
    populateService(objPath);

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

template <typename T>
mctp_vdm::requester::Coroutine APResource<T>::updateHealthAsync()
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

template <typename T>
bool APResource<T>::isERoTHealthy() const noexcept
{
    return erotResource->isDeviceEnumerated() and
           erotResource->checkForEnabledMCTPEids();
}

template <typename T>
bool APResource<T>::isAPInRecovery() const noexcept
{
    if (isERoTHealthy())
    {
        auto status = erotResource->getBootStatus();

        auto bootCompleteTimeout = (status[status.size() - 4]) & (1 << 3);

        return static_cast<bool>(bootCompleteTimeout);
    }
    return !isApHealthy();
}

#ifdef MCTP_IN_KERNEL
using TRequest = mctp_vdm::requester::InKernelRequest;
#else
using TRequest = mctp_vdm::requester::DaemonRequest;
#endif

// Explicit template instantiations
template class APResource<TRequest>;
