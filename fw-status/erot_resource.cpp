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

#include "erot_resource.hpp"

ERoTResource::ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                           sdeventplus::Event& event, const uint64_t i2cBus,
                           const uint64_t i2cAddress, uint8_t eid,
                           uint8_t apEid, const std::string chassisObjPath,
                           const std::string apObjPath,
                           const bool isRecoverable,
                           std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
    MCTPDiscoveryResource(bus, objPath, eid), sdEvent(event),
    mctpVdmHelper(mctpVdmHelper), isRecoverable(isRecoverable)
{
    glacierRecoveryObj =
        std::make_unique<glacier_recovery_tool::glacier_recovery_commands::
                             GlacierRecoveryCommands>(i2cBus, i2cAddress,
                                                      false);
    bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
    bootStatus->bootStatus({0});
    bootStatus->bootStatusType(
        BootStatusServer::BootStatusTypes::ERoTBootStatus);
    apResource = std::make_unique<APResource>(bus, apObjPath, apEid, this);

    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);

    apBootStatusTimer =
        std::make_unique<sdbusplus::Timer>(sdEvent.get(), [this, objPath]() {
            lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
            updateBootStatusAsync().detach();
        });
}

ERoTResource::ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                           sdeventplus::Event& event, uint8_t eid,
                           const std::string chassisObjPath,
                           const bool isRecoverable,
                           std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
    MCTPDiscoveryResource(bus, objPath, eid), sdEvent(event),
    mctpVdmHelper(mctpVdmHelper), isRecoverable(isRecoverable)
{
    bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
    bootStatus->bootStatus({0});
    bootStatus->bootStatusType(
        BootStatusServer::BootStatusTypes::ERoTBootStatus);

    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);

    apBootStatusTimer =
        std::make_unique<sdbusplus::Timer>(sdEvent.get(), [this, objPath]() {
            lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
            updateBootStatusAsync().detach();
        });
}

bool ERoTResource::isApBootFinished(const std::vector<uint8_t>& status)
{
    bool isApBootCompleted =
        nvidia::fw_status::boot_status::isAPBootComplete(status);
    bool isApBootCompleteTimeout =
        nvidia::fw_status::boot_status::isAPBootCompleteTimeout(status);
    auto fatalErrorCode =
        nvidia::fw_status::boot_status::getAPFatalErrorCode(status);

    if (isApBootCompleteTimeout)
    {
        lg2::error("AP boot complete timeout");
    }

    if (fatalErrorCode.has_value() && fatalErrorCode.value() != 0)
    {
        lg2::error("AP fatal error code {CODE}", "CODE",
                   static_cast<uint16_t>(fatalErrorCode.value()));
    }

    return isApBootCompleted || isApBootCompleteTimeout ||
           (fatalErrorCode.has_value() && fatalErrorCode.value() != 0);
}

void ERoTResource::updateERoTHealth()
{
    if (bootStatus)
    {
        updateBootStatus();
    }

    if (!isRecoverable)
    {
        return;
    }

    const bool mctpEnumerated = MCTPDiscoveryResource::isDeviceEnumerated();

    if (!mctpEnumerated)
    {
        if (isChassisPoweredOff())
        {
            health(HealthServer::HealthType::Warning);
            state(OperationalStatusServer::StateType::UnavailableOffline);
            return;
        }
    }

    const auto& status = glacierRecoveryObj->performInitialization();
    bool inRecovery =
        (status != glacier_recovery_tool::glacier_recovery_commands::
                       RecoveryResult::FirmwareNotInRecovery);

    if (mctpEnumerated)
    {
        lg2::info("MCTP EID for {PATH} is enumerated", "PATH", path.c_str());
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
        return;
    }

    if (inRecovery)
    {
        lg2::info("Device associated with {PATH} is in recovery", "PATH",
                  path.c_str());
        commitRecoveryModeError(fetchEid());

        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        return;
    }

    lg2::warning("Device associated with {PATH} is healthy but MCTP "
                 "connectivity is not available",
                 "PATH", path.c_str());

    health(HealthServer::HealthType::Critical);
    state(OperationalStatusServer::StateType::Degraded);
    return;
}

mctp_vdm::requester::Coroutine ERoTResource::updateBootStatusAsync()
{
    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock())
    {
        lg2::error("BootStatus refresh already in progress for EID={EID}",
                   "EID", fetchEid());
        co_return 0;
    }
    if (MCTPDiscoveryResource::isDeviceEnumerated())
    {
        const mctp_vdm::Message* responseMsg = nullptr;
        size_t responseLen = 0;
        auto eid = fetchEid();
        co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

        if (responseMsg != nullptr && responseLen > 1)
        {
            std::vector<uint8_t> status(responseMsg->payload + 1,
                                        responseMsg->payload + responseLen);
            bootStatus->bootStatus(status);

            if (!isApBootFinished(status))
            {
                apBootStatusTimer->start(std::chrono::microseconds(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::seconds(apBootCompleteRetryInterval))));
            }
        }
    }
    else
    {
        bootStatus->bootStatus({0});
    }

    co_return 0;
}

std::vector<uint8_t> ERoTResource::getBootStatus() const noexcept
{
    return bootStatus->bootStatus();
}
