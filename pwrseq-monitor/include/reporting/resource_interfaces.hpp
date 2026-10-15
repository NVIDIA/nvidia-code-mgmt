/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * D-Bus reporting glue. Adapted from the nvidia-code-mgmt
 * fw-status/base_resource.hpp::ResourceInterfaces pattern. Owns one sdbusplus
 * object per resource exposing both Health and OperationalStatus on the
 * standard openbmc inventory path; bmcweb auto-surfaces this as
 * Redfish Status.Health / Status.State / Status.Conditions[].
 *
 * Lazy-creation: the D-Bus object isn't materialised until the first write
 * happens. This matches fw-status's behavior and avoids cluttering the bus
 * with objects that never see a state update.
 */

#pragma once

#include "core/types.hpp"
#include "reporting/state_mapper.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>

#include <memory>
#include <string>

namespace pwrseq::reporting
{

using ResourceInterfacesInherit = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus>;

/** Wraps an sdbusplus object exposing Health + OperationalStatus on one
 *  D-Bus path. Direct port of the fw-status idiom.
 */
class ResourceInterfaces : public ResourceInterfacesInherit
{
  public:
    ResourceInterfaces(sdbusplus::bus::bus& bus, const std::string& objPath) :
        ResourceInterfacesInherit(bus, objPath.c_str(),
                                  action::emit_interface_added)
    {}
};

/** Per-resource reporter: holds the path, creates the D-Bus object on the
 *  first status write, and provides a single applyStatus() entry point.
 *
 *  One Reporter is owned by each FSM (Element / Module / Tray). The
 *  orchestrator writes the initial state after wiring the callback, then the
 *  callback writes every later state change. The initial write is required
 *  because a seeded input can be a no-op transition from the FSM's default
 *  state and therefore never invoke onStateChange.
 */
class Reporter
{
  public:
    Reporter(sdbusplus::bus::bus& bus, std::string objPath) :
        bus_(bus), path_(std::move(objPath))
    {}

    /** Element-state writer. `present` distinguishes Stby-but-here vs Absent. */
    void applyStatus(ElementState s, bool present);

    /** Module-state writer. */
    void applyStatus(ModuleState s);

    /** Tray-state writer. */
    void applyStatus(TrayState s);

    const std::string& path() const noexcept { return path_; }

  private:
    void ensureCreated_()
    {
        if (!resource_)
        {
            resource_ = std::make_unique<ResourceInterfaces>(bus_, path_);
        }
    }
    void writeTuple_(const StatusTuple& t)
    {
        ensureCreated_();
        resource_->health(t.health);
        resource_->state(t.state);
        resource_->functional(t.functional);
    }

    sdbusplus::bus::bus&                bus_;
    std::string                         path_;
    std::unique_ptr<ResourceInterfaces> resource_;
};

}  // namespace pwrseq::reporting
