/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * D-Bus interface for the LPU/FPGA power-sequence monitor daemon.
 *
 * Exposes methods on `/com/Nvidia/PwrSeqMonitor`, interface
 * `com.Nvidia.PwrSeqMonitor.Manager`:
 *
 *   Reset(target: string, kind: string) -> (result: string)
 *
 *     Pulse-reset one element. `target` is the descriptor id
 *     ("HGX_LPU_3", "HGX_FPGA_0", ...); `kind` is one of "Warm",
 *     "Cold", "PowerCycle".
 *
 *     Method does not reply until the post-reset BC re-asserts
 *     OR the reset-completion timeout (20s) elapses. This is the
 *     deferred-reply pattern that lets bmcweb's POST handler block on
 *     the boot completion via gpio-mon BcHi edges (no in-daemon GPIO read).
 *
 *     TEMPORARY (TS1 bring-up): the 20s reset-completion budget is raised
 *     from T_BOOT + 3s for slow post-reset BC on the bench. Revert after
 *     TS1 validation.
 *
 *     result is one of:
 *       "Ok"            -- pulse fired, BC re-asserted in time
 *       "Timeout"       -- pulse fired, BC did NOT re-assert in time
 *       "NotPresent"    -- no driver/FSM for `target`
 *       "Busy"          -- another reset for `target` already in flight
 *       "NotSupported"  -- driver rejected the ResetKind
 *       "PulseFailed"   -- driver's pulse returned non-Ok
 *       "InvalidKind"   -- `kind` string did not match {Warm,Cold,PowerCycle}
 *
 *   ResetAll(element_class: string, kind: string) -> (results: a{ss})
 *
 *     Pulse-reset every present element of a class IN PARALLEL.
 *     `element_class` is "LPU" or "FPGA"; `kind` as above.
 *     Returns a dict mapping each element's descriptor id to its
 *     result string. Method does not reply until every per-element
 *     async reset has completed (each waits up to the 20s
 *     reset-completion timeout; TEMPORARY for TS1 — revert after TS1).
 *
 *   SetVrTelemetryEnabled(target: string, enabled: bool) -> (result: string)
 *
 *     Records Entity Manager's static board-revision selection. The request
 *     never reads or changes a MUX. pwrseq combines it with Online state and
 *     firmware >= 11.3.0 before adding the VR polling bit.
 *
 * BootStatusCode is deliberately absent: Redfish assembles it from the
 * boot-status lines published by xyz.openbmc_project.Gpio.Monitor, so a
 * Processor GET never reaches this daemon. Serving those reads here meant
 * every poll turned into a synchronous gpio-mon round trip on the event
 * loop, which starved reset completion.
 *
 * The daemon acquires every reset_line as an exclusive OUTPUT at init.
 * Input PG/BC/boot-status edges arrive via Gpio.Monitor.Line
 * PropertiesChanged → injectLineState (not a D-Bus method on this object).
 */

#pragma once

#include "hal/driver_common.hpp"
#include "orchestrator.hpp"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>

#include <deque>
#include <memory>

namespace pwrseq
{

/** Inherits sdbusplus's internal bus_friend to gain access to the raw
 *  sd_bus* (needed for sd_bus_add_object_vtable / message reply APIs that
 *  sdbusplus does not wrap directly). bus_friend is the documented
 *  escape hatch for "internal users" per sdbusplus/bus.hpp.
 */
class DBusManager : private sdbusplus::bus::details::bus_friend
{
  public:
    /** `lineRegistry` is borrowed -- DBusManager does not own it. The
     *  orchestrator owns the registry; we accept it here so property
     *  injectors can look up SharedLines by name. */
    DBusManager(sdbusplus::bus::bus& bus, Orchestrator& orch,
                hal::common::SharedLineRegistry& lineRegistry);
    ~DBusManager();

    DBusManager(const DBusManager&) = delete;
    DBusManager& operator=(const DBusManager&) = delete;

    // sd-bus method dispatchers (must match sd_bus_message_handler_t).
    // Public so they can be referenced from a vtable defined in this
    // translation unit's anonymous namespace; not part of the public
    // API in the documentation sense -- treat as internal.
    static int dispatchReset(sd_bus_message*, void*, sd_bus_error*);
    static int dispatchResetAll(sd_bus_message*, void*, sd_bus_error*);
    static int dispatchSetVrTelemetryEnabled(sd_bus_message*, void*,
                                             sd_bus_error*);
    static int dispatchRequestLogDrain(sd_bus_message*, void*, sd_bus_error*);

    /** Inject a line state from an in-process source.
     *
     *  Used by the Gpio.Monitor.Line property subscriber (initial
     *  ObjectManager snapshot and subsequent PropertiesChanged). Electrical
     *  levels are deferred through the fan-out queue so burst property
     *  updates do not block the sd-event loop inside FSM work.
     *
     *  `asserted` is the electrical level (same semantics as ReadGpio /
     *  Gpio.Monitor.Line.Value). SharedLine::notifyExternal applies
     *  active-low polarity from platform.json.
     *
     *  Returns false when platform.json did not register the line.
     */
    bool injectLineState(const std::string& lineName, bool asserted,
                         const char* source);

  private:
    /** A single edge notification awaiting fan-out into subscribers.
     *  Captures the SharedLine by shared_ptr so the line lives at
     *  least until drain processes the item, even if the registry's
     *  weak_ptr expires in between (which shouldn't happen in
     *  practice but the guarantee is cheap).
     */
    struct PendingNotify
    {
        std::shared_ptr<hal::common::SharedLine> line;
        bool asserted;
    };

    /** Push an edge onto the drain queue and arm the drain source if
     *  not already armed.
     */
    void enqueueNotify_(std::shared_ptr<hal::common::SharedLine> line,
                        bool asserted);

    /** Arm (or re-arm in ONESHOT mode) the deferred drain source so
     *  the next event-loop iteration calls drainPendingNotifies_.
     *  Idempotent within a single loop iteration.
     */
    void scheduleDrain_();

    /** Defer-source callback that drains pendingQueue_ in FIFO order
     *  and calls SharedLine::notifyExternal() for each item. Runs in
     *  the same sd-event loop thread as the bus-method dispatcher,
     *  so no synchronisation is needed for the queue.
     */
    static int drainPendingNotifies_(sd_event_source* s, void* userdata);

    sdbusplus::bus::bus& bus_;
    Orchestrator& orch_;
    hal::common::SharedLineRegistry& lineRegistry_;
    sd_bus_slot* slot_ = nullptr;
    std::deque<PendingNotify> pendingQueue_;
    sd_event_source* drainSource_ = nullptr;
};

} // namespace pwrseq
