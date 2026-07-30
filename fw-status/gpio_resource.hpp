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

#include "base_resource.hpp"
#include "dbusutils.hpp"
#include "glacier_recovery_commands.hpp"
#include "mctp_discovery_resource.hpp"
#include "mctp_vdm_helper.hpp"

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

/** @class GPIOResource
 *  Represents a BaseResource whose healthy status is updated by monitoring GPIO
 */
class GPIOResource : public BaseResource
{
  public:
    enum class MonitorMode : uint8_t
    {
        Interrupt,
        Polling
    };

  private:
    enum class HealthUpdateReason : uint8_t
    {
        Refresh,
        FatalErrorAssert,
        FatalErrorDeassert
    };

  public:
    /** @brief Constructor for the GPIOResource Class - Monitoring GPIO
     * Interrupt for ERoT device Updates Health and Status of the D-Bus object
     * by monitoring ERoT FATAL_ERROR pin
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param event - sdevent
     * @param i2cBus - I2C Bus where the resource is present
     * @param i2cAddress - I2C Address of the resource
     * @param eid - MCTP Endpoint ID of the Resource
     * @param gpio - GPIO line name
     * @param target - systemd unit to be executed when ERoT is recovered
     * @param monitorMode - GPIO monitor mode
     * @param pollingIntervalMs - GPIO polling interval in milliseconds
     * @param gpioPolarity - GPIO polarity
     * @param apName - Optional AP software object name associated with this
     * ERoT
     * @param mctpVdmHelper - MCTP VDM helper object
     * @param hideWhenHealthy - Whether to hide the D-Bus object when healthy
     *
     */
    GPIOResource(sdbusplus::bus_t& bus, const std::string& objPath,
                 sdeventplus::Event& event, const uint64_t i2cBus,
                 const uint64_t i2cAddress, uint8_t eid,
                 const std::string& gpio, const std::string& target,
                 const std::string& monitorMode,
                 std::optional<uint64_t> pollingIntervalMs,
                 const std::string& gpioPolarity, const std::string& apName,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper,
                 bool hideWhenHealthy = false);

    ~GPIOResource() override;

  private:
    sdeventplus::Event& sdEvent;
    uint8_t eid;
    std::string gpioLineName;
    std::string systemTarget;
    bool isFirmwareInRecovery = false;
    bool hideWhenHealthy = false;
    int polarity;
    MonitorMode monitorMode = MonitorMode::Interrupt;
    std::chrono::milliseconds pollingInterval{0};
    std::optional<int> lastGpioValue;
    gpiod::line gpioLine;
    std::unique_ptr<sdeventplus::source::IO> gpioEvent;
    std::unique_ptr<sdbusplus::Timer> gpioRetryTimer;
    std::unique_ptr<sdbusplus::Timer> gpioPollingTimer;
    std::unique_ptr<sdbusplus::Timer> apBootStatusRetryTimer;
    std::unique_ptr<glacier_recovery_tool::glacier_recovery_commands::
                        GlacierRecoveryCommands>
        glacierRecoveryObj;
    std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
    std::unique_ptr<BaseResource> apResource;
    bool apBootStatusCheckActive = false;
    size_t apBootStatusQueryRetryCount = 0;
    std::shared_ptr<bool> apBootStatusLifetimeToken =
        std::make_shared<bool>(true);
    std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type>
        apBootStatusCo;

    /** @brief callback function to handle GPIO event
     *
     */
    void waitForGPIOEvent();

    /** @brief Disable GPIO event handling and clear the cached GPIO line.
     *
     */
    void clearGPIOEvent();

    /** @brief function to request gpio line and register callback for gpio
     * event
     *
     */
    bool registerGPIOEvent();

    /** @brief Start retrying GPIO event registration.
     *
     */
    void startGPIOEventRetry();

    /** @brief Stop retrying GPIO event registration.
     *
     */
    void stopGPIOEventRetry();

    /** @brief Try to re-register the GPIO event from the retry timer.
     *
     */
    void retryGPIOEventRegistration();

    /** @brief Start polling the GPIO input line.
     *
     */
    void startGPIOPolling();

    /** @brief Stop GPIO polling.
     *
     */
    void stopGPIOPolling();

    /** @brief Request the GPIO line as input for polling.
     *
     * @return true if the GPIO line is ready for get_value(), false otherwise.
     */
    bool requestGPIOInputLine();

    /** @brief Poll the GPIO line and update health when the value changes.
     *
     */
    void pollGpio();

    /** @brief Check whether a GPIO value matches the configured active level.
     *
     */
    bool isGPIOActive(int value) const;

    /** @brief Publish healthy status or hide the object for legacy configs.
     *
     */
    void updateHealthyState();

    /** @brief function to update Health and State of ERoT D-Bus object
     *         Uses Glacier Crisis Recovery Protocol to fetch device status
     */
    void updateERoTHealth(
        HealthUpdateReason reason = HealthUpdateReason::Refresh);

    /** @brief Whether this ERoT GPIO resource should also update AP health.
     */
    bool hasAP() const noexcept;

    /** @brief Start the AP boot-status retry loop after a short endpoint
     * settle delay.
     */
    void startAPBootStatusCheck();

    /** @brief Run one AP QueryBootStatus request if no request is in flight.
     */
    void runAPBootStatusQuery();

    /** @brief Schedule an AP QueryBootStatus request.
     */
    void scheduleAPBootStatusQuery(std::chrono::seconds delay);

    /** @brief Schedule the next AP QueryBootStatus retry.
     */
    void scheduleAPBootStatusRetry();

    /** @brief Stop the AP boot-status retry loop.
     */
    void stopAPBootStatusCheck();

    /** @brief Query and decode AP boot status via the shared ERoT/AP EID.
     */
    mctp_vdm::requester::Coroutine queryAPBootStatusAsync();

    /** @brief Apply a decoded AP boot status result.
     */
    void handleAPBootStatus(const std::vector<uint8_t>& status);

    /** @brief Handle missing/failed AP boot status response.
     */
    void handleAPBootStatusUnavailable();

    /** @brief Update AP health/state.
     */
    void updateAPHealth(HealthServer::HealthType healthValue,
                        OperationalStatusServer::StateType stateValue);

    /** @brief Delete the AP recovery object when the AP is healthy.
     */
    void deleteAPObject();

    /** @brief Commit a recovery error and mark the AP unhealthy,
     *  then stop the boot-status check.
     */
    void markAPUnhealthy();

    /** @brief Fetches EID for the resource
     *
     *  @return uint8_t - EID of the resource
     *
     */
    uint8_t fetchEid() const noexcept;

    /** @brief Updates Health and State of the resource on power state changes
     */
    void updateHealth() override
    {
        if (monitorMode == MonitorMode::Polling)
        {
            pollGpio();
        }
        else
        {
            updateERoTHealth();
        }
    }
};
