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

#pragma once

#include "com/nvidia/RoT/BootStatus/server.hpp"
#include "com/nvidia/SetRecoveryMode/server.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "xyz/openbmc_project/State/Decorator/Health/server.hpp"
#include "xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp"

#include <phosphor-logging/lg2.hpp>

#include <functional>
#include <memory>
#include <string>

using ResourceInterfacesInherit = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus>;

using BootStatusInterfaceInherit = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::RoT::server::BootStatus>;

using SetRecoveryModeInterfaceInherit = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::SetRecoveryMode>;

using HealthServer =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health;
using OperationalStatusServer = sdbusplus::xyz::openbmc_project::State::
    Decorator::server::OperationalStatus;
using BootStatusServer = sdbusplus::com::nvidia::RoT::server::BootStatus;

namespace MatchRules = sdbusplus::bus::match::rules;

/**@class ResourceInterface
 *
 *  Concrete implementation of xyz.openbmc_project.State.Decorator.Health and
 * xyz.openbmc_project.State.Decorator.OperationalStatus D-Bus interfaces
 *
 */
class ResourceInterfaces : public ResourceInterfacesInherit
{
  public:
    ResourceInterfaces(sdbusplus::bus_t& sdbus, const std::string& objPath) :
        ResourceInterfacesInherit(sdbus, objPath.c_str(),
                                  action::emit_interface_added)
    {}
};

/**@class BootStatus
 *
 *  Concrete implementation of com.nvidia.ERoT.BootStatus
 *  D-Bus interface
 *
 */
class BootStatus : public BootStatusInterfaceInherit
{
  public:
    BootStatus(sdbusplus::bus_t& sdbus, const std::string& objPath) :
        BootStatusInterfaceInherit(sdbus, objPath.c_str(),
                                   action::emit_interface_added)
    {}
};

/**@class SetRecoveryModeInterface
 *
 *  Concrete implementation of com.nvidia.SetRecoveryMode
 *  D-Bus interface for triggering device force recovery
 *
 */
class SetRecoveryModeInterface : public SetRecoveryModeInterfaceInherit
{
  public:
    /**@brief Constructor for the SetRecoveryModeInterface Class
     *
     * @param sdbus - SystemD bus to publish the object
     * @param path - Path of D-Bus object to publish
     * @param recoveryCallback - Callback function to perform the actual
     * recovery
     *
     */
    SetRecoveryModeInterface(sdbusplus::bus_t& sdbus, const std::string& path,
                             std::function<void()> recoveryCallback) :
        SetRecoveryModeInterfaceInherit(sdbus, path.c_str(),
                                        action::emit_interface_added),
        setForceRecovery(std::move(recoveryCallback))
    {}

    /**@brief D-Bus method implementation for SetRecoveryMode
     *
     * Invokes the recovery callback provided at construction time.
     * Throws InternalFailure on recovery errors.
     */
    void setRecoveryMode() override
    {
        try
        {
            setForceRecovery();
        }
        catch (const std::exception&)
        {
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InternalFailure();
        }
    }

  private:
    std::function<void()> setForceRecovery;
};

/**@class BaseResource
 *
 *  BaseResource represents a resource at the most abstract level and publishes
 * the ResourceInterfaces expected from all resources
 *
 */
class BaseResource
{
  public:
    sdbusplus::bus_t& bus;

    /**@brief Constructor for the BaseResource Class
     *
     * @param bus - SystemD bus to publish the object
     * @param objPath - Path of D-Bus object to publish
     *
     */
    BaseResource(sdbusplus::bus_t& bus, const std::string& objPath) :
        bus(bus), path(objPath)
    {}

    virtual ~BaseResource() = default;

    /** @brief Sets health of resource on D-Bus object.
     *         Creates D-Bus object if it doesn't already exists
     *
     */
    inline void health(auto health)
    {
        if (!resourceDbusObj)
        {
            resourceDbusObj = std::make_unique<ResourceInterfaces>(bus, path);
        }
        resourceDbusObj->health(health);
    }

    /** @brief Fetches health of the resource from D-Bus
     *
     */
    inline auto health()
    {
        if (!resourceDbusObj)
        {
            lg2::error("Invalid state of resource {PATH} to fetch health",
                       "PATH", path.c_str());
            throw std::runtime_error(
                "Invalid state of resource to fetch health");
        }
        return resourceDbusObj->health();
    }

    /** @brief Sets state of resource on D-Bus object.
     *         Creates D-Bus object if it doesn't already exists
     *
     */
    inline void state(auto state)
    {
        if (!resourceDbusObj)
        {
            resourceDbusObj = std::make_unique<ResourceInterfaces>(bus, path);
        }
        resourceDbusObj->state(state);
    }

    /** @brief Fetches state of the resource from D-Bus
     *
     */
    inline auto state()
    {
        if (!resourceDbusObj)
        {
            lg2::error("Invalid state of resource {PATH} to fetch state",
                       "PATH", path.c_str());
            throw std::runtime_error(
                "Invalid state of resource to fetch state");
        }
        return resourceDbusObj->state();
    }

    /** @brief Deletes the resource object on D-Bus
     *
     */
    inline void deleteDbusObject()
    {
        if (resourceDbusObj)
        {
            resourceDbusObj.reset();
        }
    }

    /** @brief Get the D-Bus object path for this resource
     *
     * @return The D-Bus object path as a string
     */
    inline std::string getObjectPath() const
    {
        return path;
    }

    /** @brief Gets the cached chassis power state
     *
     * @return std::string - Cached chassis power state
     *         (e.g., "xyz.openbmc_project.State.Chassis.PowerState.On")
     */
    inline std::string getChassisPowerState() const noexcept
    {
        return chassisPowerState;
    }

    /** @brief Updates the cached chassis power state
     */
    inline void setChassisPowerState(const std::string& currentPowerState)
    {
        chassisPowerState = currentPowerState;
    }

    /** @brief Updates the Health and State of the resource
     *  Default implementation is a no-op for resources that don't need it
     *  (e.g., USBRcmResource companion objects).
     *  MCTPDiscoveryResource overrides this as pure virtual.
     */
    virtual void updateHealth()
    {}

    /** @brief Whether this resource should be updated on chassis power state
     *  changes. Set from Entity Manager "hasChassisPowerSource" config.
     */
    inline bool hasChassisPowerSource() const noexcept
    {
        return connectedToChassisPower;
    }

    inline void setConnectedToChassis(bool connected) noexcept
    {
        connectedToChassisPower = connected;
        if (!connected)
        {
            chassisPowerState.clear();
        }
    }

    /** @brief Returns true if this resource is tied to chassis power and the
     *  chassis is currently Off.
     */
    inline bool isChassisPoweredOff() const noexcept
    {
        if (!connectedToChassisPower)
        {
            return false;
        }

        return chassisPowerState == chassisPowerOffState;
    }

    inline void setPowerOnSettling(bool settling) noexcept
    {
        powerOnSettling = settling;
    }

    inline bool isPowerOnSettling() const noexcept
    {
        return powerOnSettling;
    }

    /** @brief Commit a recovery-mode error to DeviceStatus via
     *  phosphor-logging.
     *
     *  @param eid - MCTP Endpoint ID of the device
     */
    void commitRecoveryModeError(uint8_t eid);

  protected:
    const std::string path;

  private:
    std::unique_ptr<ResourceInterfaces> resourceDbusObj;
    bool connectedToChassisPower = false;
    bool powerOnSettling = false;
    std::string chassisPowerState;
    static constexpr const char* chassisPowerOffState =
        "xyz.openbmc_project.State.Chassis.PowerState.Off";
};
