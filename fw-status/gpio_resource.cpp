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

#include <cerrno>
#include <chrono>
#include <system_error>

namespace
{
constexpr auto gpioEventRetryInterval = std::chrono::seconds(5);
constexpr auto defaultPollingInterval = std::chrono::milliseconds(1000);
constexpr auto minPollingInterval = std::chrono::milliseconds(100);

GPIOResource::MonitorMode parseMonitorMode(const std::string& monitorMode)
{
    if (monitorMode.empty() || monitorMode == "Interrupt")
    {
        return GPIOResource::MonitorMode::Interrupt;
    }

    if (monitorMode == "Polling")
    {
        return GPIOResource::MonitorMode::Polling;
    }

    lg2::warning("Invalid GPIO monitor mode {MODE}. Use Interrupt as default",
                 "MODE", monitorMode);

    return GPIOResource::MonitorMode::Interrupt;
}

int parseGPIOPolarity(const std::string& gpioPolarity)
{
    if (gpioPolarity.empty() || gpioPolarity == "ActiveHigh")
    {
        return gpiod::line::ACTIVE_HIGH;
    }

    if (gpioPolarity == "ActiveLow")
    {
        return gpiod::line::ACTIVE_LOW;
    }

    lg2::warning(
        "Invalid type for GPIO polarity {TYPE}. Use ActiveHigh as default",
        "TYPE", gpioPolarity);
    return gpiod::line::ACTIVE_HIGH;
}

std::chrono::milliseconds
    getPollingInterval(std::optional<uint64_t> pollingIntervalMs)
{
    if (!pollingIntervalMs.has_value())
    {
        return defaultPollingInterval;
    }

    if (pollingIntervalMs.value() == 0)
    {
        lg2::warning("PollingIntervalMs is 0. Use {VALUE} ms as default",
                     "VALUE", defaultPollingInterval.count());
        return defaultPollingInterval;
    }

    auto interval = std::chrono::milliseconds(pollingIntervalMs.value());
    if (interval < minPollingInterval)
    {
        lg2::warning(
            "PollingIntervalMs {VALUE} ms is too small. Clamp to {MIN} ms",
            "VALUE", interval.count(), "MIN", minPollingInterval.count());
        return minPollingInterval;
    }

    return interval;
}
} // namespace

GPIOResource::GPIOResource(sdbusplus::bus_t& bus, const std::string& objPath,
                           sdeventplus::Event& event, const uint64_t i2cBus,
                           const uint64_t i2cAddress, uint8_t eid,
                           const std::string& gpio, const std::string& target,
                           const std::string& monitorModeConfig,
                           std::optional<uint64_t> pollingIntervalMs,
                           const std::string& gpioPolarity,
                           bool hideWhenHealthy) :
    BaseResource(bus, objPath), sdEvent(event), eid(eid), gpioLineName(gpio),
    systemTarget(target), isEROT(true), hideWhenHealthy(hideWhenHealthy),
    polarity(parseGPIOPolarity(gpioPolarity)),
    monitorMode(parseMonitorMode(monitorModeConfig)),
    pollingInterval(getPollingInterval(pollingIntervalMs))
{
    glacierRecoveryObj =
        std::make_unique<glacier_recovery_tool::glacier_recovery_commands::
                             GlacierRecoveryCommands>(i2cBus, i2cAddress,
                                                      false);

    lg2::info("GPIO {GPIO} monitor mode: {MODE}", "GPIO", gpioLineName, "MODE",
              monitorMode == MonitorMode::Polling ? "Polling" : "Interrupt");

    if (monitorMode == MonitorMode::Polling)
    {
        startGPIOPolling();
    }
    else if (!registerGPIOEvent())
    {
        startGPIOEventRetry();
    }
}

GPIOResource::GPIOResource(
    sdbusplus::bus_t& bus, const std::string& objPath,
    sdeventplus::Event& event, uint8_t eid, const std::string& gpio,
    const std::string& risingTarget, const std::string& fallingTarget,
    const std::string& gpioPolarity, const std::string chassisObjPath,
    std::shared_ptr<MCTPVdmHelper> mctpVdmHelper, bool hideWhenHealthy) :
    BaseResource(bus, objPath), sdEvent(event), eid(eid), gpioLineName(gpio),
    risingTarget(risingTarget), fallingTarget(fallingTarget),
    hideWhenHealthy(hideWhenHealthy), polarity(parseGPIOPolarity(gpioPolarity)),
    mctpVdmHelper(mctpVdmHelper)
{
    if (!chassisObjPath.empty())
    {
        bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
        bootStatus->bootStatus({0});
        bootStatus->bootStatusType(
            BootStatusServer::BootStatusTypes::ERoTBootStatus);
    }

    initAPHealth();

    if (!registerGPIOEvent())
    {
        startGPIOEventRetry();
    }
}

GPIOResource::~GPIOResource()
{
    stopGPIOPolling();
    stopGPIOEventRetry();
    clearGPIOEvent();

    if (co && co.done())
    {
        co.destroy();
    }
}

void GPIOResource::waitForGPIOEvent()
{
    try
    {
        lineEvent = gpioLine.event_read();
    }
    catch (const std::system_error& e)
    {
        if (e.code().value() == ENODEV)
        {
            lg2::warning(
                "GPIO line {GPIO} is no longer available: {ERROR}. Disabling GPIO event source.",
                "GPIO", gpioLineName, "ERROR", e.what());
            clearGPIOEvent();
            startGPIOEventRetry();
            return;
        }

        throw;
    }

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

void GPIOResource::clearGPIOEvent()
{
    if (gpioEvent)
    {
        try
        {
            gpioEvent->set_enabled(sdeventplus::source::Enabled::Off);
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "Failed to disable GPIO event source for {GPIO}: {ERR}", "GPIO",
                gpioLineName, "ERR", e.what());
        }

        gpioEvent.reset();
    }

    if (gpioLine)
    {
        try
        {
            gpioLine.release();
        }
        catch (const std::exception& e)
        {
            lg2::warning("Failed to release GPIO line {GPIO}: {ERR}", "GPIO",
                         gpioLineName, "ERR", e.what());
        }

        gpioLine = gpiod::line{};
    }

    lastGpioValue.reset();
}

bool GPIOResource::registerGPIOEvent()
{
    lg2::info("Registering... event callback for {GPIO}", "GPIO", gpioLineName);
    gpioLine = gpiod::find_line(gpioLineName);
    if (!gpioLine)
    {
        lg2::error("Failed to find the {GPIO} line", "GPIO", gpioLineName);
        clearGPIOEvent();
        return false;
    }

    try
    {
        gpioLine.request(
            {"fw-status", gpiod::line_request::EVENT_BOTH_EDGES, {}});
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to request events for {GPIO}: {ERROR}", "GPIO",
                   gpioLineName, "ERROR", e.what());
        gpioLine = gpiod::line{};
        clearGPIOEvent();
        return false;
    }

    auto gpioLineFd = gpioLine.event_get_fd();
    if (gpioLineFd < 0)
    {
        lg2::error("Failed to get {GPIO} fd", "GPIO", gpioLineName);
        clearGPIOEvent();
        return false;
    }
    gpioEvent = std::make_unique<sdeventplus::source::IO>(
        sdEvent, gpioLineFd, EPOLLIN,
        std::bind(&GPIOResource::waitForGPIOEvent, this));
    gpioEvent->set_enabled(sdeventplus::source::Enabled::On);
    return true;
}

void GPIOResource::startGPIOEventRetry()
{
    if (!gpioRetryTimer)
    {
        gpioRetryTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(),
            std::bind(&GPIOResource::retryGPIOEventRegistration, this));
    }

    if (gpioRetryTimer->isRunning())
    {
        return;
    }

    try
    {
        gpioRetryTimer->start(gpioEventRetryInterval, true);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to start GPIO event retry timer for {GPIO}: {ERR}",
                   "GPIO", gpioLineName, "ERR", e.what());
    }
}

void GPIOResource::stopGPIOEventRetry()
{
    if (!gpioRetryTimer || !gpioRetryTimer->isRunning())
    {
        return;
    }

    auto rc = gpioRetryTimer->stop();
    if (rc)
    {
        lg2::error("Failed to stop GPIO event retry timer for {GPIO}. RC={RC}",
                   "GPIO", gpioLineName, "RC", rc);
    }
}

void GPIOResource::retryGPIOEventRegistration()
{
    if (gpioEvent)
    {
        stopGPIOEventRetry();
        return;
    }

    if (!registerGPIOEvent())
    {
        return;
    }

    stopGPIOEventRetry();
    updateHealth();
}

void GPIOResource::startGPIOPolling()
{
    lg2::info("Starting GPIO polling for {GPIO} every {INTERVAL_MS} ms", "GPIO",
              gpioLineName, "INTERVAL_MS", pollingInterval.count());

    if (!gpioPollingTimer)
    {
        gpioPollingTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(), std::bind(&GPIOResource::pollGpio, this));
    }

    if (!requestGPIOInputLine())
    {
        lg2::warning(
            "Initial acquisition of GPIO {GPIO} failed; polling will retry",
            "GPIO", gpioLineName);
    }

    if (gpioPollingTimer->isRunning())
    {
        return;
    }

    try
    {
        gpioPollingTimer->start(pollingInterval, true);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to start GPIO polling timer for {GPIO}: {ERR}",
                   "GPIO", gpioLineName, "ERR", e.what());
    }
}

void GPIOResource::stopGPIOPolling()
{
    if (!gpioPollingTimer || !gpioPollingTimer->isRunning())
    {
        return;
    }

    auto rc = gpioPollingTimer->stop();
    if (rc)
    {
        lg2::error("Failed to stop GPIO polling timer for {GPIO}. RC={RC}",
                   "GPIO", gpioLineName, "RC", rc);
    }
}

bool GPIOResource::requestGPIOInputLine()
{
    if (gpioLine)
    {
        return true;
    }

    gpioLine = gpiod::find_line(gpioLineName);
    if (!gpioLine)
    {
        lg2::error("Failed to find the {GPIO} line", "GPIO", gpioLineName);
        return false;
    }

    try
    {
        gpioLine.request(
            {"fw-status", gpiod::line_request::DIRECTION_INPUT, {}});
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to request line for {GPIO}: {ERROR}", "GPIO",
                   gpioLineName, "ERROR", e.what());
        gpioLine = gpiod::line{};
        return false;
    }

    return true;
}

void GPIOResource::pollGpio()
{
    if (!requestGPIOInputLine())
    {
        return;
    }

    int value = 0;
    try
    {
        value = gpioLine.get_value();
    }
    catch (const std::system_error& e)
    {
        if (e.code().value() == ENODEV)
        {
            lg2::warning(
                "GPIO line {GPIO} is no longer available while polling: {ERROR}",
                "GPIO", gpioLineName, "ERROR", e.what());
        }
        else
        {
            lg2::error("Failed to read GPIO {GPIO} while polling: {ERROR}",
                       "GPIO", gpioLineName, "ERROR", e.what());
        }

        clearGPIOEvent();
        return;
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to read GPIO {GPIO} while polling: {ERROR}", "GPIO",
                   gpioLineName, "ERROR", e.what());
        clearGPIOEvent();
        return;
    }

    const auto previousValue = lastGpioValue;
    const bool changed =
        previousValue.has_value() && previousValue.value() != value;
    lastGpioValue = value;

    if (!previousValue.has_value())
    {
        updateERoTHealth();
        return;
    }

    if (changed)
    {
        updateERoTHealth();
    }
}

bool GPIOResource::isGPIOActive(int value) const
{
    return (value && polarity == gpiod::line::ACTIVE_HIGH) ||
           (!value && polarity == gpiod::line::ACTIVE_LOW);
}

void GPIOResource::updateHealthyState()
{
    if (hideWhenHealthy)
    {
        deleteDbusObject();
        return;
    }

    health(HealthServer::HealthType::OK);
    state(OperationalStatusServer::StateType::Enabled);
}

void GPIOResource::updateERoTHealth()
{
    if (isChassisPoweredOff())
    {
        health(HealthServer::HealthType::Warning);
        state(OperationalStatusServer::StateType::UnavailableOffline);
        return;
    }

    const auto& status = glacierRecoveryObj->performInitialization();
    bool inRecovery =
        (status != glacier_recovery_tool::glacier_recovery_commands::
                       RecoveryResult::FirmwareNotInRecovery);

    if (inRecovery)
    {
        lg2::info("Device associated with {PATH} is in recovery", "PATH",
                  path.c_str());
        commitRecoveryModeError(fetchEid());

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
    updateHealthyState();
    return;
}

void GPIOResource::updateAPHealth(uint8_t type)
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
            try
            {
                val = gpioLine.get_value();
            }
            catch (const std::system_error& e)
            {
                if (e.code().value() == ENODEV)
                {
                    lg2::warning(
                        "GPIO line {GPIO} is no longer available while reading level: {ERROR}",
                        "GPIO", gpioLineName, "ERROR", e.what());
                }
                else
                {
                    lg2::error("Failed to read GPIO {GPIO} level: {ERROR}",
                               "GPIO", gpioLineName, "ERROR", e.what());
                }

                clearGPIOEvent();
                return;
            }
            catch (const std::exception& e)
            {
                lg2::error("Failed to read GPIO {GPIO} level: {ERROR}", "GPIO",
                           gpioLineName, "ERROR", e.what());
                clearGPIOEvent();
                return;
            }

            if (isGPIOActive(val))
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
        updateHealthyState();
        lg2::info("Device associated with {OBJ} is healthy", "OBJ",
                  path.c_str());
    }
    else
    {
        commitRecoveryModeError(fetchEid());
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);
        lg2::info("Device associated with {OBJ} needs recovery", "OBJ",
                  path.c_str());
    }
}

void GPIOResource::initAPHealth()
{
    lg2::info("Initializing... {OBJ} status", "OBJ", path.c_str());
    if (!requestGPIOInputLine())
    {
        return;
    }

    updateAPHealth(LEVEL_TRIGGER);

    if (gpioLine)
    {
        gpioLine.release();
        gpioLine = gpiod::line{};
    }
}

mctp_vdm::requester::Coroutine GPIOResource::updateBootStatusAsync()
{
    auto eid = fetchEid();

    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock())
    {
        lg2::error("BootStatus refresh already in progress for EID={EID}",
                   "EID", eid);
        co_return 0;
    }

    const mctp_vdm::Message* responseMsg = nullptr;
    size_t responseLen = 0;
    co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

    if (responseMsg != nullptr && responseLen > 1)
    {
        std::vector<uint8_t> status(responseMsg->payload + 1,
                                    responseMsg->payload + responseLen);
        bootStatus->bootStatus(status);
    }

    co_return 0;
}

uint8_t GPIOResource::fetchEid() const noexcept
{
    return eid;
}
