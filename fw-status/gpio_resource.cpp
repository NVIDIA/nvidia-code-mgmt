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

#include "boot_status_utils.hpp"

#include <cerrno>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace
{
constexpr auto gpioEventRetryInterval = std::chrono::seconds(5);
constexpr auto defaultPollingInterval = std::chrono::milliseconds(1000);
constexpr auto minPollingInterval = std::chrono::milliseconds(100);
constexpr auto apBootStatusEndpointReadyDelay = std::chrono::seconds(1);
constexpr auto apBootStatusQueryRetryInterval = std::chrono::seconds(1);
constexpr size_t maxAPBootStatusQueryRetries = 10;
constexpr auto erotRecoveryMonitorInterval = std::chrono::seconds(60);

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

std::string formatBootStatus(const std::vector<uint8_t>& status)
{
    if (status.empty())
    {
        return "<empty>";
    }

    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < status.size(); ++i)
    {
        if (i != 0)
        {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned>(status[i]);
    }

    return stream.str();
}

std::string formatFatalErrorCode(uint8_t code)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex
           << static_cast<unsigned>(code & 0x0F);
    return stream.str();
}

const char* fatalErrorStateString(bool asserted)
{
    return asserted ? "asserted" : "deasserted";
}

} // namespace

GPIOResource::GPIOResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                           sdeventplus::Event& event, const uint64_t i2cBus,
                           const uint64_t i2cAddress, uint8_t eid,
                           const std::string& gpio, const std::string& target,
                           const std::string& monitorModeConfig,
                           std::optional<uint64_t> pollingIntervalMs,
                           const std::string& gpioPolarity,
                           const std::string& apName,
                           std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
    BaseResource(bus, objPath), sdEvent(event), eid(eid), gpioLineName(gpio),
    systemTarget(target), polarity(parseGPIOPolarity(gpioPolarity)),
    monitorMode(parseMonitorMode(monitorModeConfig)),
    pollingInterval(getPollingInterval(pollingIntervalMs)),
    mctpVdmHelper(mctpVdmHelper)
{
    glacierRecoveryObj =
        std::make_unique<glacier_recovery_tool::glacier_recovery_commands::
                             GlacierRecoveryCommands>(i2cBus, i2cAddress,
                                                      false);

    if (!apName.empty())
    {
        apResource = std::make_unique<BaseResource>(
            bus, "/xyz/openbmc_project/software/" + apName);
    }

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

GPIOResource::~GPIOResource()
{
    stopGPIOPolling();
    stopGPIOEventRetry();
    stopAPBootStatusCheck();
    stopERoTRecoveryMonitor();
    apBootStatusLifetimeToken.reset();
    clearGPIOEvent();

    if (apBootStatusCo && apBootStatusCo.done())
    {
        apBootStatusCo.destroy();
    }
    else if (apBootStatusCo)
    {
        /*
         * SendRecvMctpVdmMsg does not expose a request cancellation API and its
         * response callback points into the suspended coroutine frame. Keep the
         * frame alive until the request completes, then let final_suspend destroy
         * it after queryAPBootStatusAsync observes the expired lifetime token.
         */
        apBootStatusCo.promise().detached = true;
    }
    apBootStatusCo = nullptr;
}

void GPIOResource::waitForGPIOEvent()
{
    gpiod::line_event lineEvent;
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

    const bool fatalErrorAsserted =
        (lineEvent.event_type == gpiod::line_event::RISING_EDGE &&
         polarity == gpiod::line::ACTIVE_HIGH) ||
        (lineEvent.event_type == gpiod::line_event::FALLING_EDGE &&
         polarity == gpiod::line::ACTIVE_LOW);
    lg2::info("GPIO {GPIO} FATAL_ERROR {STATE} on {EDGE} edge", "GPIO",
              gpioLineName, "STATE", fatalErrorStateString(fatalErrorAsserted),
              "EDGE",
              lineEvent.event_type == gpiod::line_event::RISING_EDGE ? "rising"
                                                                     : "falling");
    updateERoTHealth(fatalErrorAsserted
                         ? HealthUpdateReason::FatalErrorAssert
                         : HealthUpdateReason::FatalErrorDeassert);
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
        clearGPIOEvent();
        return false;
    }

    try
    {
        const auto value = gpioLine.get_value();
        if (isGPIOActive(value))
        {
            lg2::info(
                "GPIO {GPIO} FATAL_ERROR asserted during initial interrupt read, value={VALUE}",
                "GPIO", gpioLineName, "VALUE", value);
            updateERoTHealth(HealthUpdateReason::FatalErrorAssert);
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning("Failed to read initial value for {GPIO}: {ERROR}", "GPIO",
                     gpioLineName, "ERROR", e.what());
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
        if (isGPIOActive(value))
        {
            lg2::info(
                "GPIO {GPIO} FATAL_ERROR asserted during initial polling read, value={VALUE}",
                "GPIO", gpioLineName, "VALUE", value);
            updateERoTHealth(HealthUpdateReason::FatalErrorAssert);
        }
        return;
    }

    if (changed)
    {
        const bool fatalErrorAsserted = isGPIOActive(value);
        lg2::info(
            "GPIO {GPIO} FATAL_ERROR {STATE} by polling, previous={PREVIOUS}, current={CURRENT}",
            "GPIO", gpioLineName, "STATE",
            fatalErrorStateString(fatalErrorAsserted), "PREVIOUS",
            previousValue.value(), "CURRENT", value);
        updateERoTHealth(fatalErrorAsserted
                             ? HealthUpdateReason::FatalErrorAssert
                             : HealthUpdateReason::FatalErrorDeassert);
    }
}

bool GPIOResource::isGPIOActive(int value) const
{
    return (value && polarity == gpiod::line::ACTIVE_HIGH) ||
           (!value && polarity == gpiod::line::ACTIVE_LOW);
}

void GPIOResource::updateERoTHealth(GPIOResource::HealthUpdateReason reason)
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

        // Only commit the recovery error on the transition into recovery. The
        // ERoT recovery monitor and health refreshes can re-enter this path
        // while still in recovery, and re-committing would duplicate the error.
        if (!isFirmwareInRecovery)
        {
            commitRecoveryModeError(fetchEid());
        }

        isFirmwareInRecovery = true;
        health(HealthServer::HealthType::Critical);
        state(OperationalStatusServer::StateType::StandbyOffline);

        // In polling mode the brief FATAL_ERROR deassert when the ERoT exits
        // recovery cannot be observed (it re-asserts immediately if the AP is
        // also unhealthy). Poll the MCTP endpoint instead to detect the ERoT
        // coming back. Interrupt mode sees both edges, so it does not need this.
        startERoTRecoveryMonitor();
        return;
    }

    lg2::info("Device associated with {PATH} is not in recovery", "PATH",
              path.c_str());

    bool systemTargetRestarted = false;

    if (reason == HealthUpdateReason::FatalErrorDeassert)
    {
        stopAPBootStatusCheck();
        deleteAPObject();
    }

    if (isFirmwareInRecovery)
    {
        stopERoTRecoveryMonitor();
        lg2::info("{OBJ} exits the recovery mode", "OBJ", path);
        if (systemTarget.empty())
        {
            lg2::warning(
                "No MCTP init target configured for recovered ERoT {OBJ}",
                "OBJ", path);
        }
        else
        {
            auto newBus = sdbusplus::bus::new_default();
            auto dbusUtil = nvidia::software::updater::DBUSUtils(newBus);
            lg2::info("Restarting {TARGET}...", "TARGET", systemTarget);
            /*
             *  Some ERoTs on the same bus which share the same target
             *  Restart the target to ensure the later one can trigger
             *  the MCTP re-discovery
             */
            dbusUtil.restartSystemUnit(systemTarget);
            systemTargetRestarted = true;
        }
        isFirmwareInRecovery = false;
    }
    deleteDbusObject();

    if (reason == HealthUpdateReason::FatalErrorAssert && hasAP())
    {
        if (systemTarget.empty())
        {
            lg2::warning(
                "No MCTP init target configured for AP of {OBJ}; HMC ERoT endpoint may be missing before AP boot-status query",
                "OBJ", path);
        }
        else if (!systemTargetRestarted)
        {
            auto newBus = sdbusplus::bus::new_default();
            auto dbusUtil = nvidia::software::updater::DBUSUtils(newBus);
            lg2::info(
                "Restarting {TARGET} to initialize HMC ERoT endpoint before AP boot-status query",
                "TARGET", systemTarget);
            dbusUtil.restartSystemUnit(systemTarget);
        }
        else
        {
            lg2::info(
                "Skipping restart of {TARGET} before AP boot-status query because it was already restarted after ERoT recovery",
                "TARGET", systemTarget);
        }
        startAPBootStatusCheck();
    }

    return;
}

bool GPIOResource::hasAP() const noexcept
{
    return apResource != nullptr;
}

void GPIOResource::startAPBootStatusCheck()
{
    if (!hasAP())
    {
        return;
    }

    if (!mctpVdmHelper)
    {
        lg2::warning(
            "AP configured for {OBJ}, but MCTP VDM helper is unavailable",
            "OBJ", path);
        return;
    }

    stopAPBootStatusCheck();
    apBootStatusQueryRetryCount = 0;
    apBootStatusCheckActive = true;
    /*
     * FATAL_ERROR can be observed before the HMC ERoT endpoint is ready,
     * especially right after restarting the target that initializes it. Always
     * give discovery a short settle window before the first QueryBootStatus; any
     * later failures use the normal retry loop.
     */
    lg2::info(
        "Waiting {DELAY_MS} ms for HMC ERoT endpoint before AP boot-status query for {OBJ}",
        "DELAY_MS",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            apBootStatusEndpointReadyDelay)
            .count(),
        "OBJ", apResource->getObjectPath());
    scheduleAPBootStatusQuery(apBootStatusEndpointReadyDelay);
}

void GPIOResource::runAPBootStatusQuery()
{
    if (!apBootStatusCheckActive || !hasAP() || !mctpVdmHelper)
    {
        return;
    }

    if (apBootStatusCo && !apBootStatusCo.done())
    {
        lg2::info("AP boot-status query already in progress for {OBJ}", "OBJ",
                  apResource->getObjectPath());
        return;
    }

    if (apBootStatusCo)
    {
        apBootStatusCo.destroy();
        apBootStatusCo = nullptr;
    }

    auto rc = queryAPBootStatusAsync();
    apBootStatusCo = rc.handle;

    if (apBootStatusCo.done())
    {
        apBootStatusCo = nullptr;
    }
}

void GPIOResource::scheduleAPBootStatusQuery(std::chrono::seconds delay)
{
    if (!apBootStatusCheckActive || !hasAP())
    {
        return;
    }

    if (!apBootStatusRetryTimer)
    {
        apBootStatusRetryTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(),
            std::bind(&GPIOResource::runAPBootStatusQuery, this));
    }

    try
    {
        apBootStatusRetryTimer->start(delay, false);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to start AP boot-status query timer for {OBJ}: {ERR}",
            "OBJ", apResource->getObjectPath(), "ERR", e.what());
    }
}

void GPIOResource::scheduleAPBootStatusRetry()
{
    scheduleAPBootStatusQuery(apBootStatusQueryRetryInterval);
}

void GPIOResource::stopAPBootStatusCheck()
{
    apBootStatusCheckActive = false;
    apBootStatusQueryRetryCount = 0;

    if (!apBootStatusRetryTimer || !apBootStatusRetryTimer->isRunning())
    {
        return;
    }

    auto rc = apBootStatusRetryTimer->stop();
    if (rc)
    {
        lg2::error(
            "Failed to stop AP boot-status retry timer for {OBJ}. RC={RC}",
            "OBJ",
            apResource ? apResource->getObjectPath()
                                : std::string{},
            "RC", rc);
    }
}

mctp_vdm::requester::Coroutine GPIOResource::queryAPBootStatusAsync()
{
    auto eid = fetchEid();
    auto mctpVdmHelperRef = mctpVdmHelper;
    std::weak_ptr<bool> lifetimeToken(apBootStatusLifetimeToken);
    if (!mctpVdmHelperRef)
    {
        co_return 0;
    }

    const mctp_vdm::Message* responseMsg = nullptr;
    size_t responseLen = 0;
    auto rc = co_await mctpVdmHelperRef->queryBootStatus(eid, responseMsg,
                                                         responseLen);

    if (lifetimeToken.expired())
    {
        co_return rc;
    }

    if (!apBootStatusCheckActive || !hasAP())
    {
        co_return 0;
    }

    if (rc != 0 || responseMsg == nullptr || responseLen <= 1)
    {
        lg2::warning(
            "Failed to query AP boot status for {OBJ}, EID={EID}, RC={RC}",
            "OBJ", apResource->getObjectPath(), "EID", eid, "RC", rc);
        handleAPBootStatusUnavailable();
        co_return rc;
    }

    std::vector<uint8_t> status(responseMsg->payload + 1,
                                responseMsg->payload + responseLen);
    handleAPBootStatus(status);

    co_return 0;
}

void GPIOResource::handleAPBootStatus(const std::vector<uint8_t>& status)
{
    const auto bootStatus = formatBootStatus(status);
    const auto fatalErrorCode =
        nvidia::fw_status::boot_status::getAPFatalErrorCode(status);

    if (!fatalErrorCode.has_value())
    {
        lg2::warning(
            "AP boot status for {OBJ}: {STATUS}; FATAL_ERROR_CODE unavailable, missing bits 28-31",
            "OBJ", apResource->getObjectPath(), "STATUS",
            bootStatus);
        handleAPBootStatusUnavailable();
        return;
    }

    lg2::info("AP boot status for {OBJ}: {STATUS}; FATAL_ERROR_CODE={CODE}",
              "OBJ", apResource->getObjectPath(), "STATUS",
              bootStatus, "CODE", formatFatalErrorCode(fatalErrorCode.value()));

    if (fatalErrorCode.value() != 0)
    {
        lg2::info("AP associated with {OBJ} has fatal error code {CODE}", "OBJ",
                  apResource->getObjectPath(), "CODE",
                  formatFatalErrorCode(fatalErrorCode.value()));
        markAPUnhealthy();
        return;
    }

    // No fatal error code. A boot-complete timeout still means the AP failed to
    // finish booting, so treat it as unhealthy.
    if (nvidia::fw_status::boot_status::isAPBootCompleteTimeout(status))
    {
        lg2::info("AP associated with {OBJ} hit boot-complete timeout", "OBJ",
                  apResource->getObjectPath());
        markAPUnhealthy();
        return;
    }

    // No fatal error and boot completed => AP is healthy.
    if (nvidia::fw_status::boot_status::isAPBootComplete(status))
    {
        lg2::info("AP associated with {OBJ} completed boot with no fatal error",
                  "OBJ", apResource->getObjectPath());
        deleteAPObject();
        stopAPBootStatusCheck();
        return;
    }

    // No fatal error yet and boot has not completed. Retry briefly because the
    // FATAL_ERROR-triggered MCTP path may not be ready immediately.
    lg2::info("AP associated with {OBJ} is still booting", "OBJ",
              apResource->getObjectPath());
    handleAPBootStatusUnavailable();
}

void GPIOResource::markAPUnhealthy()
{
    apResource->commitRecoveryModeError(fetchEid());
    updateAPHealth(HealthServer::HealthType::Critical,
                            OperationalStatusServer::StateType::StandbyOffline);
    stopAPBootStatusCheck();
}

void GPIOResource::handleAPBootStatusUnavailable()
{
    if (!apBootStatusCheckActive || !hasAP())
    {
        return;
    }

    if (apBootStatusQueryRetryCount >= maxAPBootStatusQueryRetries)
    {
        lg2::warning(
            "AP boot status did not reach a terminal state after {COUNT} retries for {OBJ}",
            "COUNT", maxAPBootStatusQueryRetries, "OBJ",
            apResource->getObjectPath());
        updateAPHealth(HealthServer::HealthType::Critical,
                                OperationalStatusServer::StateType::Degraded);
        stopAPBootStatusCheck();
        return;
    }

    ++apBootStatusQueryRetryCount;
    lg2::info("Retrying AP boot status query for {OBJ}. Retry {RETRY}/{MAX}",
              "OBJ", apResource->getObjectPath(), "RETRY",
              apBootStatusQueryRetryCount, "MAX", maxAPBootStatusQueryRetries);
    scheduleAPBootStatusRetry();
}

void GPIOResource::updateAPHealth(
    HealthServer::HealthType healthValue,
    OperationalStatusServer::StateType stateValue)
{
    if (!apResource)
    {
        return;
    }

    apResource->health(healthValue);
    apResource->state(stateValue);
}

void GPIOResource::deleteAPObject()
{
    if (apResource)
    {
        apResource->deleteDbusObject();
    }
}

void GPIOResource::startERoTRecoveryMonitor()
{
    // Only polling mode needs this. Interrupt mode observes both the deassert
    // and re-assert edges via the kernel event FIFO.
    if (monitorMode != MonitorMode::Polling)
    {
        return;
    }

    erotRecoveryMonitorActive = true;

    // React the moment the ERoT MCTP endpoint appears instead of waiting for the
    // next timer tick. The match is kept alive and gated by
    // erotRecoveryMonitorActive so it is never destroyed from within its own
    // callback.
    if (!erotEndpointAddedMatch)
    {
        erotEndpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            MatchRules::interfacesAdded(mctpObjMgrPath.data()) +
                MatchRules::sender(mctpService),
            [this](sdbusplus::message::message& msg) {
                onERoTEndpointAdded(msg);
            });
    }

    if (!erotRecoveryMonitorTimer)
    {
        erotRecoveryMonitorTimer = std::make_unique<sdbusplus::Timer>(
            sdEvent.get(),
            std::bind(&GPIOResource::runERoTRecoveryMonitor, this));
    }

    if (!erotRecoveryMonitorTimer->isRunning())
    {
        lg2::info(
            "Starting ERoT recovery monitor for {OBJ} every {INTERVAL_S} s",
            "OBJ", path, "INTERVAL_S", erotRecoveryMonitorInterval.count());
        try
        {
            erotRecoveryMonitorTimer->start(erotRecoveryMonitorInterval, true);
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to start ERoT recovery monitor timer for {OBJ}: {ERR}",
                "OBJ", path, "ERR", e.what());
        }
    }

    // Kick MCTP discovery now rather than waiting for the first timer tick. The
    // endpoint will be picked up by the interfacesAdded match above. Do not
    // re-evaluate health here: this runs inside updateERoTHealth().
    triggerMctpDiscovery();
}

void GPIOResource::triggerMctpDiscovery()
{
    // A directly-attached ERoT needs a target restart to (re)trigger MCTP
    // discovery. An ERoT behind an MCTP bridge has no target configured and its
    // endpoint appears on its own. Restarting the target does not disturb the
    // separate glacier recovery I2C.
    if (systemTarget.empty())
    {
        return;
    }

    try
    {
        auto newBus = sdbusplus::bus::new_default();
        auto dbusUtil = nvidia::software::updater::DBUSUtils(newBus);
        lg2::info(
            "Restarting {TARGET} to trigger MCTP discovery for recovering ERoT {OBJ}",
            "TARGET", systemTarget, "OBJ", path);
        dbusUtil.restartSystemUnit(systemTarget);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to restart {TARGET} for recovering ERoT {OBJ}: {ERR}",
                   "TARGET", systemTarget, "OBJ", path, "ERR", e.what());
    }
}

void GPIOResource::onERoTEndpointAdded(sdbusplus::message::message& msg)
{
    if (!erotRecoveryMonitorActive)
    {
        return;
    }

    try
    {
        sdbusplus::message::object_path addedPath;
        nvidia::software::updater::InterfaceMap interfaces;
        msg.read(addedPath, interfaces);

        if (!interfaces.contains(mctpEndpointIntfName))
        {
            return;
        }

        const auto* mctpEID = std::get_if<uint8_t>(
            &interfaces.at(mctpEndpointIntfName).at("EID"));
        if (!mctpEID || (*mctpEID != eid))
        {
            return;
        }

        // The ERoT MCTP endpoint is back, so glacier recovery has completed and
        // the glacier I2C is free again. Re-evaluate health, which clears the
        // ERoT object and hands off to the AP boot-status check.
        lg2::info(
            "MCTP endpoint for ERoT {OBJ} (EID={EID}) added; re-evaluating health",
            "OBJ", path, "EID", eid);
        reevaluateERoTHealthAfterRecovery();
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to process ERoT MCTP interfacesAdded signal for {OBJ}: {ERROR}",
            "OBJ", path, "ERROR", e.what());
    }
}

void GPIOResource::stopERoTRecoveryMonitor()
{
    erotRecoveryMonitorActive = false;

    if (!erotRecoveryMonitorTimer || !erotRecoveryMonitorTimer->isRunning())
    {
        return;
    }

    auto rc = erotRecoveryMonitorTimer->stop();
    if (rc)
    {
        lg2::error(
            "Failed to stop ERoT recovery monitor timer for {OBJ}. RC={RC}",
            "OBJ", path, "RC", rc);
    }
}

void GPIOResource::reevaluateERoTHealthAfterRecovery()
{
    // The ERoT is back on MCTP, but the FATAL_ERROR line may already have
    // deasserted (e.g. the AP is healthy). Read the current level so we do not
    // force an assert flow and an unnecessary AP boot-status diagnosis when the
    // pin is inactive. Fall back to asserted if the level cannot be read.
    HealthUpdateReason reason = HealthUpdateReason::FatalErrorAssert;
    try
    {
        std::optional<int> value;
        if (gpioLine)
        {
            value = gpioLine.get_value();
        }
        else if (lastGpioValue.has_value())
        {
            value = lastGpioValue;
        }

        if (value.has_value())
        {
            reason = isGPIOActive(value.value())
                         ? HealthUpdateReason::FatalErrorAssert
                         : HealthUpdateReason::FatalErrorDeassert;
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "Failed to read GPIO {GPIO} level after ERoT recovery; assuming asserted: {ERR}",
            "GPIO", gpioLineName, "ERR", e.what());
    }

    updateERoTHealth(reason);
}

void GPIOResource::runERoTRecoveryMonitor()
{
    if (!erotRecoveryMonitorActive)
    {
        return;
    }

    // Fallback path: the interfacesAdded match normally detects the endpoint
    // first. If the endpoint is already present here (e.g. the add signal was
    // missed), re-evaluate health directly. It is safe to read glacier now
    // because the endpoint being up means recovery has completed.
    if (isMctpEndpointPresent())
    {
        lg2::info(
            "MCTP endpoint for ERoT {OBJ} (EID={EID}) is present; re-evaluating health",
            "OBJ", path, "EID", eid);
        reevaluateERoTHealthAfterRecovery();
        return;
    }

    // Endpoint not up yet; re-trigger MCTP discovery and wait for the endpoint
    // to appear (handled by the interfacesAdded match or the next tick).
    lg2::info("MCTP endpoint for ERoT {OBJ} (EID={EID}) not present yet; "
              "re-triggering discovery",
              "OBJ", path, "EID", eid);
    triggerMctpDiscovery();
}

bool GPIOResource::isMctpEndpointPresent()
{
    try
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
            if (mctpEID && (*mctpEID == eid))
            {
                return true;
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning("Failed to query MCTP endpoints for {OBJ}: {ERR}", "OBJ",
                     path, "ERR", e.what());
    }

    return false;
}

uint8_t GPIOResource::fetchEid() const noexcept
{
    return eid;
}
