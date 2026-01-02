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

#include "gpio_resource.hpp"

template <typename T>
GPIOResource<T>::GPIOResource(sdbusplus::bus::bus& bus,
                              const std::string& objPath,
                              sdeventplus::Event& event, const uint64_t i2cBus,
                              const uint64_t i2cAddress,
                              const std::string& uuid, const std::string& gpio,
                              const std::string& target) :
    BaseResource(bus, objPath), sdEvent(event), uuid(uuid), gpioLineName(gpio),
    systemTarget(target), isEROT(true)
{
    isFirmwareInRecovery = false;
    glacierRecoveryObj =
        std::make_unique<glacier_recovery_tool::glacier_recovery_commands::
                             GlacierRecoveryCommands>(i2cBus, i2cAddress,
                                                      false);

    registerGPIOEvent();

    // Call it one time to initialize the status
    updateERoTHealth();
}

template <typename T>
GPIOResource<T>::GPIOResource(
    sdbusplus::bus::bus& bus, const std::string& objPath,
    sdeventplus::Event& event, const std::string& uuid, const std::string& gpio,
    const std::string& risingTarget, const std::string& fallingTarget,
    const std::string& gpioPolarity, const std::string chassisObjPath,
    std::shared_ptr<MCTPVdmHelper<T>> mctpVdmHelper) :
    BaseResource(bus, objPath), sdEvent(event), uuid(uuid), gpioLineName(gpio),
    risingTarget(risingTarget), fallingTarget(fallingTarget), isEROT(false),
    mctpVdmHelper(mctpVdmHelper)
{
    if (!chassisObjPath.empty())
    {
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatus({0});
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::ERoTBootStatus);
    }

    if (gpioPolarity == "ActiveHigh")
    {
        polarity = gpiod::line::ACTIVE_HIGH;
    }
    else if (gpioPolarity == "ActiveLow")
    {
        polarity = gpiod::line::ACTIVE_LOW;
    }
    else
    {
        lg2::error(
            "Invalid type for GPIO polarity {TYPE}. Use ActiveHigh as default",
            "TYPE", gpioPolarity);
        polarity = gpiod::line::ACTIVE_HIGH;
    }

    initAPHealth();

    registerGPIOEvent();
}

template <typename T>
void GPIOResource<T>::waitForGPIOEvent()
{
    lineEvent = gpioLine.event_read();
    if (lineEvent.event_type == gpiod::line_event::RISING_EDGE)
        lg2::info("{GPIO} is rising", "GPIO", gpioLineName);
    else
        lg2::info("{GPIO} is falling", "GPIO", gpioLineName);
    if (isEROT)
    {
        updateERoTHealth();
    }
    else
    {
        updateAPHealth(EDGE_TRIGGER);
    }
}

template <typename T>
void GPIOResource<T>::registerGPIOEvent()
{
    lg2::info("Registering... event callback for {GPIO}", "GPIO", gpioLineName);
    gpioLine = gpiod::find_line(gpioLineName);
    if (!gpioLine)
    {
        lg2::error("Failed to find the {GPIO} line", "GPIO", gpioLineName);
        return;
    }

    try
    {
        gpioLine.request(
            {"fw-status", gpiod::line_request::EVENT_BOTH_EDGES, {}});
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to request events for {GPIO}: {ERROR}", "GPIO",
                   gpioLineName, "ERROR", e);
        return;
    }

    auto gpioLineFd = gpioLine.event_get_fd();
    if (gpioLineFd < 0)
    {
        lg2::error("Failed to get {GPIO} fd", "GPIO", gpioLineName);
        return;
    }
    gpioEvent = std::make_unique<sdeventplus::source::IO>(
        sdEvent, gpioLineFd, EPOLLIN,
        std::bind(&GPIOResource::waitForGPIOEvent, this));
    gpioEvent->set_enabled(sdeventplus::source::Enabled::On);
}

template <typename T>
void GPIOResource<T>::updateERoTHealth()
{
    const auto& status = glacierRecoveryObj->performInitialization();

    if (status != glacier_recovery_tool::glacier_recovery_commands::
                      RecoveryResult::FirmwareNotInRecovery)
    {
        lg2::info("Device associated with {PATH} is in recovery", "PATH",
                  path.c_str());

        isFirmwareInRecovery = true;
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        return;
    }

    lg2::info("Device associated with {PATH} is not in recovery", "PATH",
              path.c_str());

    if (isFirmwareInRecovery)
    {
        lg2::info("{OBJ} exits the recovery mode", "OBJ", path);
        auto newBus = sdbusplus::bus::new_default();
        auto dbusUtil = nvidia::software::updater::DBUSUtils(newBus);
        lg2::info("Restarting {TARGET}...", "TARGET", systemTarget);
        /*
         *  Some ERoTs on the same bus which share the same target
         *  Restart the target to ensure the later one can trigger
         *  the MCTP re-discovery
         */
        dbusUtil.restartSystemUnit(systemTarget);
        isFirmwareInRecovery = false;
    }
    deleteDbusObject();
    return;
}

template <typename T>
void GPIOResource<T>::updateAPHealth(uint8_t type)
{
    bool healthy = false;
    int val;
    auto newBus = sdbusplus::bus::new_default();
    auto dbusUtil = nvidia::software::updater::DBUSUtils(newBus);

    if (bootStatus)
    {
        updateBootStatus();
    }

    // Use event type to check AP health
    switch (type)
    {
        case EDGE_TRIGGER:
            if ((lineEvent.event_type == gpiod::line_event::RISING_EDGE &&
                 polarity == gpiod::line::ACTIVE_HIGH) ||
                (lineEvent.event_type == gpiod::line_event::FALLING_EDGE &&
                 polarity == gpiod::line::ACTIVE_LOW))
            {
                healthy = true;
            }
            else
            {
                healthy = false;
            }

            if (!risingTarget.empty() &&
                (lineEvent.event_type == gpiod::line_event::RISING_EDGE))
            {
                lg2::info("Starting... {TARGET}", "TARGET", risingTarget);
                dbusUtil.startSystemUnit(risingTarget);
            }
            else if (!fallingTarget.empty() &&
                     (lineEvent.event_type == gpiod::line_event::FALLING_EDGE))
            {
                lg2::info("Starting... {TARGET}", "TARGET", fallingTarget);
                dbusUtil.startSystemUnit(fallingTarget);
            }
            break;
        case LEVEL_TRIGGER:
            val = gpioLine.get_value();
            if ((val && polarity == gpiod::line::ACTIVE_HIGH) ||
                (!val && polarity == gpiod::line::ACTIVE_LOW))
            {
                healthy = true;

                if (polarity == gpiod::line::ACTIVE_HIGH &&
                    !risingTarget.empty())
                {
                    lg2::info("Starting... {TARGET}", "TARGET", risingTarget);
                    dbusUtil.startSystemUnit(risingTarget);
                }
                else if (polarity == gpiod::line::ACTIVE_LOW &&
                         !fallingTarget.empty())
                {
                    lg2::info("Starting... {TARGET}", "TARGET", fallingTarget);
                    dbusUtil.startSystemUnit(fallingTarget);
                }
            }
            else
            {
                healthy = false;

                // Trigger the falling target when its polarity is ACTIVE_HIGH
                // (i.e., high is healthy)
                if (polarity == gpiod::line::ACTIVE_HIGH &&
                    !fallingTarget.empty())
                {
                    lg2::info("Starting... {TARGET}", "TARGET", fallingTarget);
                    dbusUtil.startSystemUnit(fallingTarget);
                }
                else if (polarity == gpiod::line::ACTIVE_LOW &&
                         !risingTarget.empty())
                {
                    lg2::info("Starting... {TARGET}", "TARGET", risingTarget);
                    dbusUtil.startSystemUnit(risingTarget);
                }
            }
            break;
        default:
            lg2::error("Invalid type {TYPE}", "TYPE", type);
    }

    if (healthy)
    {
        deleteDbusObject();
        lg2::info("Device associated with {OBJ} is healthy", "OBJ",
                  path.c_str());
    }
    else
    {
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        lg2::info("Device associated with {OBJ} needs recovery", "OBJ",
                  path.c_str());
    }
}

template <typename T>
void GPIOResource<T>::initAPHealth()
{
    lg2::info("Initializing... {OBJ} status", "OBJ", path.c_str());
    gpioLine = gpiod::find_line(gpioLineName);
    if (!gpioLine)
    {
        lg2::error("Failed to find the {GPIO} line", "GPIO", gpioLineName);
        return;
    }

    try
    {
        gpioLine.request(
            {"fw-status", gpiod::line_request::DIRECTION_INPUT, {}});
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to request line for {GPIO}: {ERROR}", "GPIO",
                   gpioLineName, "ERROR", e);
        return;
    }
    updateAPHealth(LEVEL_TRIGGER);

    gpioLine.release();
}

template <typename T>
mctp_vdm::requester::Coroutine GPIOResource<T>::updateBootStatusAsync()
{
    auto eid = fetchEid();
    if (eid == invalidEid)
    {
        lg2::error("Cannot get EID for {PATH}", "PATH", path);
        if (mctpObjManagerMatch.empty())
        {
            mctpObjManagerMatch.emplace_back(
                bus, MatchRules::interfacesAdded(mctpObjMgrPath.data()),
                [&]([[maybe_unused]] sdbusplus::message::message& msg) {
                    updateBootStatus();
                });
        }
        co_return 0;
    }

    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock())
    {
        lg2::error("BootStatus refresh already in progress for EID={EID}",
                   "EID", eid);
        co_return 0;
    }

    mctpObjManagerMatch.clear();

    const mctp_vdm::Message* responseMsg = nullptr;
    size_t responseLen = 0;
    co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

    if (responseMsg != nullptr)
    {
        std::vector<uint8_t> status(responseMsg->payload + 1,
                                    responseMsg->payload + responseLen);
        bootStatus->bootStatus(status);
    }

    co_return 0;
}

template <typename T>
uint8_t GPIOResource<T>::fetchEid() const noexcept
{
    // Get MCTP endpoint list
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

    // Check if it has UUID interface and if it is same as the UUID from JSON
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
    {
        for (const auto& [service, interfaces] : mapperServiceMap)
        {
            auto it =
                std::find(interfaces.begin(), interfaces.end(), uuidIntfName);
            if (it != interfaces.end())
            {
                auto mctpUuid = dbusUtil.getProperty<std::string>(
                    service.c_str(), objPath.c_str(), uuidIntfName, "UUID");
                if (mctpUuid == uuid)
                {
                    return std::stoi(std::filesystem::path(objPath).stem());
                }
            }
        }
    }

    return invalidEid;
}

#ifdef MCTP_IN_KERNEL
using TRequest = mctp_vdm::requester::InKernelRequest;
#else
using TRequest = mctp_vdm::requester::DaemonRequest;
#endif

// Explicit template instantiations
template class GPIOResource<TRequest>;
