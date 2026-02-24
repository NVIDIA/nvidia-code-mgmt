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
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <memory>
#include <unordered_set>

/** @class GPIOResource
 *  Represents a BaseResource whose healthy status is updated by monitoring GPIO
 */
class GPIOResource : public BaseResource
{
    enum : uint8_t
    {
        LEVEL_TRIGGER = 0,
        EDGE_TRIGGER
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
     *
     */
    GPIOResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 sdeventplus::Event& event, const uint64_t i2cBus,
                 const uint64_t i2cAddress, uint8_t eid,
                 const std::string& gpio, const std::string& target);

    /** @brief Constructor for the GPIOResource Class - Monitoring GPIO
     * Interrupt for Non-ERoT devices Updates Health and Status of the D-Bus
     * object by monitoring specified GPIO ready pin
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     * @param event - sdevent
     * @param eid - MCTP Endpoint ID of the Resource
     * @param gpio - GPIO line name
     * @param risingTarget - systemd unit to be executed when rising event
     * triggered
     * @param fallingTarget - systemd unit to be executed when risifallingng
     * event triggered
     * @param gpioPolarity - GPIO polarity
     * @param chassisObjPath - Path of the Chassis D-Bus object to publish
     * BootStatus
     * @param mctpVdmHelper - MCTP VDM helper object
     */
    GPIOResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 sdeventplus::Event& event, uint8_t eid,
                 const std::string& gpio, const std::string& risingTarget,
                 const std::string& fallingTarget,
                 const std::string& gpioPolarity,
                 const std::string chassisObjPath,
                 std::shared_ptr<MCTPVdmHelper> mctpVdmHelper);

  private:
    sdeventplus::Event& sdEvent;
    uint8_t eid;
    std::string gpioLineName;
    std::string systemTarget;
    std::string risingTarget;
    std::string fallingTarget;
    bool isFirmwareInRecovery;
    bool isEROT;
    int polarity;
    gpiod::line gpioLine;
    gpiod::line_event lineEvent;
    std::unique_ptr<sdeventplus::source::IO> gpioEvent;
    std::unique_ptr<glacier_recovery_tool::glacier_recovery_commands::
                        GlacierRecoveryCommands>
        glacierRecoveryObj;
    std::mutex mtx;
    std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
    std::unique_ptr<BootStatus> bootStatus;
    std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type> co;

    /** @brief callback function to handle GPIO event
     *
     */
    void waitForGPIOEvent();

    /** @brief function to request gpio line and register callback for gpio
     * event
     *
     */
    void registerGPIOEvent();

    /** @brief function to update Health and State of ERoT D-Bus object
     *         Uses Glacier Crisis Recovery Protocol to fetch device status
     */
    void updateERoTHealth();

    /** @brief function to update Health and State of AP D-Bus object.
     *         Uses GPIO event or GPIO value to determine the status of AP
     */
    void updateAPHealth(uint8_t type);

    /** @brief function to read GPIO value and call updateAPHealth() to
     * initialize its status
     */
    void initAPHealth();

    /** @brief Updates the BootStatus of the AP on chassis D-Bus object
     *
     * @return coroutine
     *
     */
    mctp_vdm::requester::Coroutine updateBootStatusAsync();

    /** @brief Updates the BootStatus D-Bus object
     *
     * @return coroutine
     *
     */
    void updateBootStatus()
    {
        if (co)
        {
            co = nullptr;
        }
        auto rc = updateBootStatusAsync();
        co = rc.handle;

        if (co.done())
        {
            co = nullptr;
        }
    }

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
        if (isEROT)
        {
            updateERoTHealth();
        }
        else if (isChassisPoweredOff())
        {
            health(HealthServer::HealthType::Warning);
            state(OperationalStatusServer::StateType::UnavailableOffline);
        }
        else
        {
            updateAPHealth(LEVEL_TRIGGER);
        }
    }
};
