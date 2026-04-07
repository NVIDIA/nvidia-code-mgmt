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

void APResource::updateHealth()
{
    if (co)
    {
        if (co.done())
        {
            co.destroy();
        }
        else
        {
            co.promise().detached = true;
        }
        co = nullptr;
    }

    auto rc = updateHealthAsync(erotResource->isChassisPoweredOff());
    co = rc.handle;
    rc.handle = nullptr;

    if (co.done())
    {
        co.destroy();
        co = nullptr;
    }
}

mctp_vdm::requester::Coroutine APResource::initializeHealth()
{
    if (erotResource->isChassisPoweredOff())
    {
        health(HealthServer::HealthType::Warning);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        co_return 0;
    }

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

    if (!isERoTHealthy())
    {
        lg2::error(
            "Unable to read AP device status: ERoT not available and AP MCTP "
            "not enumerated");
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        co_return 0;
    }

    if (isAPInRecovery())
    {
        timer = std::make_unique<sdbusplus::Timer>([&]() {
            updateHealthAsync(erotResource->isChassisPoweredOff()).detach();
        });

        timer->start(std::chrono::seconds(maxBootCompleteTimeout), false);
    }
    else
    {
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }

    co_return 0;
}

mctp_vdm::requester::Coroutine
    APResource::updateHealthAsync(bool chassisPoweredOff)
{
    if (chassisPoweredOff)
    {
        health(HealthServer::HealthType::Warning);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        co_return 0;
    }

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

    if (!isERoTHealthy())
    {
        lg2::error(
            "Unable to read AP device status: ERoT not available and AP MCTP "
            "not enumerated");
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        co_return 0;
    }

    if (isAPInRecovery())
    {
        commitRecoveryModeError(fetchEid());
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
    }
    else
    {
        lg2::warning("AP MCTP EID not available but device not in recovery");
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::Degraded);
    }

    co_return 0;
}

bool APResource::isERoTHealthy() const noexcept
{
    return erotResource->isDeviceEnumerated();
}

bool APResource::isAPInRecovery() const noexcept
{
    if (isERoTHealthy())
    {
        auto status = erotResource->getBootStatus();
        return getBit(status, AP0_BOOT_COMPLETE_TIMEOUT_BIT);
    }
    return !isApHealthy();
}
