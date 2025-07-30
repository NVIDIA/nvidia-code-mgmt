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

template <typename T>
ERoTResource<T>::ERoTResource(
    sdbusplus::bus::bus& bus, const std::string& objPath,
    sdeventplus::Event& event, const uint64_t i2cBus, const uint64_t i2cAddress,
    const std::string& uuid, const uint64_t apEid,
    const std::string chassisObjPath, const std::string apObjPath,
    const bool isRecoverable, std::shared_ptr<MCTPVdmHelper<T>> mctpVdmHelper) :
    MCTPDiscoveryResource(bus, objPath, uuid), sdEvent(event),
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
    apResource = std::make_unique<APResource<T>>(bus, apObjPath, apEid, this);

    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);

    updateERoTHealth();

    apBootStatusTimer =
        std::make_unique<sdbusplus::Timer>(sdEvent.get(), [this, objPath]() {
            lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
            updateBootStatusAsync().detach();
        });
}

template <typename T>
ERoTResource<T>::ERoTResource(sdbusplus::bus::bus& bus,
                              const std::string& objPath,
                              sdeventplus::Event& event,
                              const std::string& uuid,
                              const std::string chassisObjPath,
                              const bool isRecoverable,
                              std::shared_ptr<MCTPVdmHelper<T>> mctpVdmHelper) :
    MCTPDiscoveryResource(bus, objPath, uuid), sdEvent(event),
    mctpVdmHelper(mctpVdmHelper), isRecoverable(isRecoverable)
{
    bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
    bootStatus->bootStatus({0});
    bootStatus->bootStatusType(
        BootStatusServer::BootStatusTypes::ERoTBootStatus);

    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);

    updateERoTHealth();

    apBootStatusTimer =
        std::make_unique<sdbusplus::Timer>(sdEvent.get(), [this, objPath]() {
            lg2::info("Checking Boot Status of {OBJ}", "OBJ", objPath);
            updateBootStatusAsync().detach();
        });
}

template <typename T>
bool ERoTResource<T>::isApBootFinished(const std::vector<uint8_t>& status)
{
    bool isApBootCompleted = getBit(status, AP0_BOOT_COMPLETE_BIT);
    bool isApBootCompleteTimeout =
        getBit(status, AP0_BOOT_COMPLETE_TIMEOUT_BIT);

    if (isApBootCompleteTimeout)
    {
        lg2::error("AP boot complete timeout");
    }

    return isApBootCompleted || isApBootCompleteTimeout;
}

template <typename T>
void ERoTResource<T>::updateERoTHealth()
{
    if (bootStatus)
    {
        updateBootStatus();
    }

    if (!isRecoverable)
    {
        return;
    }

    if (MCTPDiscoveryResource::isDeviceEnumerated() and
        MCTPDiscoveryResource::checkForEnabledMCTPEids())
    {
        lg2::info("MCTP EID for {PATH} is enumerated and enabled", "PATH",
                  path.c_str());
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
        return;
    }

    if (!glacierRecoveryObj->unlockI2CDevice())
    {
        lg2::error("Unable to unlock I2C for object {OBJECT}", "OBJECT",
                   path.c_str());
        health(HealthServer::HealthType::Critical);
        if (MCTPDiscoveryResource::isDeviceEnumerated())
        {
            state(OperationalStatusServer::StateType::UnavailableOffline);
            return;
        }

        state(OperationalStatusServer::StateType::Absent);
        return;
    }

    const auto& status = glacierRecoveryObj->performInitialization();

    if (status != glacier_recovery_tool::glacier_recovery_commands::
                      RecoveryResult::FirmwareNotInRecovery)
    {
        lg2::info("Device associated with {PATH} is in recovery", "PATH",
                  path.c_str());

        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        return;
    }

    lg2::info("Device associated with {PATH} is not in recovery", "PATH",
              path.c_str());
    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);
    return;
}

template <typename T>
mctp_vdm::requester::Coroutine ERoTResource<T>::updateBootStatusAsync()
{
    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock())
    {
        lg2::error("BootStatus refresh already in progress for EID={EID}",
                   "EID", fetchEid());
        co_return 0;
    }
    if (MCTPDiscoveryResource::isDeviceEnumerated() and
        MCTPDiscoveryResource::checkForEnabledMCTPEids())
    {
        const mctp_vdm::Message* responseMsg = nullptr;
        size_t responseLen = 0;
        auto eid = fetchEid();
        co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

        if (responseMsg != nullptr)
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

template <typename T>
std::vector<uint8_t> ERoTResource<T>::getBootStatus() const noexcept
{
    return bootStatus->bootStatus();
}

#ifdef MCTP_IN_KERNEL
using TRequest = mctp_vdm::requester::InKernelRequest;
#else
using TRequest = mctp_vdm::requester::DaemonRequest;
#endif

// Explicit template instantiations
template class ERoTResource<TRequest>;
