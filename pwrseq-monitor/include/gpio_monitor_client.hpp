/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Subscribe to xyz.openbmc_project.Gpio.Monitor.Line property updates and
 * seed/live-feed PG/BC/boot-status SharedLines via DBusManager::injectLineState.
 */

#pragma once

#include "config/descriptor.hpp"
#include "dbus_manager.hpp"

#include <sdbusplus/bus.hpp>

#include <memory>

namespace pwrseq
{

/** Owns D-Bus matches for the daemon lifetime (opaque Impl). */
class GpioPropertySubscriber
{
  public:
    GpioPropertySubscriber(sdbusplus::bus::bus& bus,
                           const config::PlatformDescriptor& desc,
                           DBusManager& manager);
    ~GpioPropertySubscriber();

    GpioPropertySubscriber(const GpioPropertySubscriber&) = delete;
    GpioPropertySubscriber& operator=(const GpioPropertySubscriber&) = delete;
    GpioPropertySubscriber(GpioPropertySubscriber&&) noexcept;
    GpioPropertySubscriber& operator=(GpioPropertySubscriber&&) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Install PropertiesChanged (+ ObjectManager) matches, then kick an
 *  *asynchronous* GetManagedObjects snapshot.
 *
 *  Async (not sync sd_bus_call) is required: a blocking call queues match
 *  signals until after the reply and can replay stale Value deltas on top
 *  of the snapshot. FIFO processing keeps live edges and the snapshot
 *  reply ordered. Transient GetManagedObjects failures while the monitor
 *  may be present are retried with a bounded backoff; missing-name waits
 *  for NameOwnerChanged.
 *
 *  Bound Value updates for platform.json input lines go through
 *  injectLineState (polarity/FSM unchanged). Non-power-sequence and
 *  unbound lines are ignored. ListGpios/ReadGpio remain for boot-status.
 */
inline GpioPropertySubscriber startGpioPropertySubscriber(
    sdbusplus::bus::bus& bus, const config::PlatformDescriptor& desc,
    DBusManager& manager)
{
    return GpioPropertySubscriber(bus, desc, manager);
}

} // namespace pwrseq
