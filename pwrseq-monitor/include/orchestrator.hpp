/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * The orchestrator owns the FSM hierarchy + HAL drivers + reporters and
 * wires everything together at daemon startup. Topology comes from a
 * PlatformDescriptor (config/descriptor.hpp), not from hardcoded counts —
 * see design §6, "descriptor-driven instantiation".
 */

#pragma once

#include "config/descriptor.hpp"
#include "fsm/element_fsm.hpp"
#include "fsm/module_fsm.hpp"
#include "fsm/tray_fsm.hpp"
#include "hal/driver_common.hpp"
#include "hal/fpga_driver.hpp"
#include "hal/i_device.hpp"
#include "hal/lpu_driver.hpp"
#include "reporting/resource_interfaces.hpp"

#include <sdbusplus/bus.hpp>
#include <systemd/sd-event.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pwrseq
{

/** SemanticEventSink + EventLoop integration owner.
 *
 *  The orchestrator routes events from HAL drivers up through Element FSMs
 *  → Module FSMs → Tray FSM, and forwards Redfish reset requests down
 *  through the HAL as one-shot device.reset() calls.
 *
 *  All FSM transitions are processed on the daemon's single sd-event thread;
 *  no internal locking required.
 */
class Orchestrator : public hal::SemanticEventSink
{
  public:
    Orchestrator(sdbusplus::bus::bus& bus, sd_event* loop);
    ~Orchestrator() override;

    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    /** Build the FSM hierarchy + HAL drivers + reporters from a descriptor.
     *  Returns Result::Ok if every driver initialised; on partial failure,
     *  drivers that DID initialise remain wired and the failed slots are
     *  logged and reflected as StandbyOffline in their Module FSMs.
     *
     *  Replaces the previous initLp30Inventory(). Adding a new tray variant
     *  is now a JSON edit (files/platform.json) — no recompile.
     */
    hal::Result initFromDescriptor(const config::PlatformDescriptor& desc);

    /** Implementation of SemanticEventSink. Dispatches an event to the right
     *  Element/Module/Tray FSM based on its target InstanceId.
     */
    void emit(const SemanticEvent& ev) override;

    /** Trigger a Redfish-initiated reset on a specific element. Dispatches
     *  RstAsserted to the FSM, invokes the device's one-shot reset (LSTP
     *  assert-only trigger on LP30), then dispatches RstReleased — the FSM
     *  re-arms T_BOOT on its own.
     *
     *  Synchronous: returns the driver-level result of the reset trigger
     *  (Ok/HwError/NotSupported) but does NOT wait for the post-reset
     *  BcHi. Used internally by requestResetAsync below for the
     *  trigger-and-FSM-dispatch portion; external callers should prefer
     *  the async variant so the Redfish POST waits for BC.
     */
    hal::Result requestReset(InstanceId target, hal::ResetKind kind);

    /** Trigger an immediate LPU log drain for an Online hardware event. */
    hal::Result requestLogDrain(InstanceId target, std::string_view reason);

    /** Result of an asynchronous reset request, delivered to the callback. */
    enum class ResetOutcome : uint8_t
    {
        Ok,             // BC re-asserted within the deadline
        Timeout,        // Reset trigger OK but BC did not re-assert in time
        NotPresent,     // No driver / FSM for that target
        Busy,           // A reset for this target is already in flight
        NotSupported,   // Driver returned NotSupported
        PulseFailed,    // Driver's reset() returned non-Ok
    };

    /** Async variant: triggers reset + waits for BcHi (or the
     *  reset-completion timeout) before invoking the callback. Suitable for
     *  D-Bus method handlers that need to defer the reply until BC is observed.
     *
     *  TEMPORARY (TS1): reset-completion timeout is 20s; revert to T_BOOT + 3s
     *  after TS1 validation.
     *
     *  Behaviour:
     *   1. Reject (Busy) if a reset for `target` is already pending.
     *   2. Reject (NotPresent) if the target has no driver/FSM.
     *   3. Synchronously: dispatch RstAsserted, trigger driver reset,
     *      dispatch RstReleased. If the driver's reset() returns non-Ok,
     *      complete the callback immediately with PulseFailed.
     *   4. Register a deadline timer (default: t_boot_ms + 3000 ms slack).
     *   5. The next BcHi observed for `target` (via the normal HAL ->
     *      Orchestrator::emit path) completes the callback with Ok.
     *   6. If the deadline fires first, completes with Timeout.
     *
     *  Re-entrancy: callback runs on the same sd-event thread that
     *  owns the orchestrator. Safe to invoke further requestResetAsync
     *  calls from inside it.
     */
    void requestResetAsync(
        InstanceId target,
        hal::ResetKind kind,
        std::function<void(ResetOutcome)> callback);

    /** Batched class-wide reset: trigger every present element of `cls`
     *  near-simultaneously and wait for each to reach BcHi in parallel.
     *
     *  Sequence:
     *    1. Snapshot presentInstances(cls).
     *    2. For each target, dispatch RstAsserted to its FSM and call
     *       device->resetAssert(kind) (LSTP assert-only trigger).
     *    3. For each successful assert, dispatch RstReleased (software;
     *       module MCU clears the reset net on LP30).
     *    4. Install per-element BC-wait deadline identical to
     *       requestResetAsync (BcHi via gpio-mon Line PropertiesChanged).
     *    5. As each element completes, accumulate into a per-element
     *       <descriptorId, ResetOutcome> map. When the last completes,
     *       fire `callback` once with the full map.
     *
     *  Why batched: each driver->resetAssert is a single libgpiod ioctl
     *  (~tens of microseconds). Issuing asserts back-to-back collapses
     *  inter-element trigger skew to the per-ioctl noise floor.
     *
     *  Drivers that don't override resetAssert (default returns
     *  NotSupported) fall back to per-element requestResetAsync for that
     *  target, which still works -- just without the skew reduction.
     *  The outcome map still gets populated correctly.
     */
    void requestResetAllSync(
        InstanceId::Class cls,
        hal::ResetKind kind,
        std::function<void(std::vector<std::pair<std::string, ResetOutcome>>)>
            callback);

    /** Borrowed access to the SharedLineRegistry. Exposed so DBusManager
     *  / the GPIO property subscriber can inject edges into the right
     *  SharedLine's subscriber list. Reference outlives DBusManager
     *  (both owned by main() with the orchestrator constructed first
     *  and destructed last). */
    hal::common::SharedLineRegistry& lines() noexcept { return sharedLines_; }

    /** Resolve a user-facing descriptor id (e.g. "HGX_LPU_3",
     *  "HGX_FPGA_0") to its internal InstanceId. Returns std::nullopt
     *  when the id is not registered (sparse module / typo / future
     *  hot-add). Case-sensitive: bmcweb passes the descriptor.id
     *  verbatim, and the descriptor's id is authoritative.
     */
    std::optional<InstanceId> instanceByDescriptorId(
        const std::string& id) const;

    /** Reverse of instanceByDescriptorId(): looks up the user-facing
     *  descriptor id for an internal InstanceId. Returns empty string
     *  for an unknown InstanceId. */
    std::string descriptorIdByInstance(InstanceId id) const;

    /** Enumerate all element InstanceIds of a given class that have a
     *  live driver/FSM (i.e. init succeeded). Used by ResetAll to know
     *  which elements to dispatch to on lp30-bench, where 12 of 16 LPU
     *  slots are sparsely populated. Returned in walk order (matches
     *  descriptor order).
     */
    std::vector<InstanceId> presentInstances(InstanceId::Class cls) const;

    /** Apply Entity Manager's static C00/C02 VR telemetry selection for one
     *  LPU. pwrseq remains the sole owner of the lpu850 polling-mask ioctl. */
    hal::Result setVrTelemetryConfigured(InstanceId target, bool enable);

    /** Write a point-in-time FSM + line-cache snapshot to `path`.
     *  Invoked from SIGUSR1 so dreport can collect it into a Manager dump.
     *  Atomic (tmp + rename). Safe on the sd-event thread. */
    bool dumpState(const std::string& path) const;

  private:
    /** Reset dispatch gate: wired element that has handled at least one PgHi. */
    bool resetEligible_(InstanceId id) const;
    // FSM wiring ────────────────────────────────────────────────────────────
    void wireElementFsm_(fsm::ElementFsm& fsm, hal::IDevice& dev,
                         reporting::Reporter& rep, bool isLpu);
    void wireModuleFsm_(fsm::ModuleFsm& fsm, reporting::Reporter& rep);
    void wireTrayFsm_(fsm::TrayFsm& fsm, reporting::Reporter& rep);

    // Descriptor → DeviceConfig conversion. Non-const because the produced
    // DeviceConfig carries a non-const pointer to sharedLines_ — drivers
    // mutate the registry by acquiring lines.
    hal::DeviceConfig buildLpuConfig_(InstanceId id,
                                      const config::ElementDescriptor& el);
    hal::DeviceConfig buildFpgaConfig_(InstanceId id,
                                       const config::FpgaDescriptor& f);

    // sd-event glue ─────────────────────────────────────────────────────────
    void registerDeviceFds_(hal::IDevice& dev);
    void onDeviceReadable_(hal::IDevice* dev, int fd);

    void startBootTimer_(InstanceId id);
    void cancelBootTimer_(InstanceId id);
    void scheduleTelemetryRetry_(InstanceId id, unsigned attempt);
    void cancelTelemetryRetry_(InstanceId id);
    void handleTelemetryRetry_(InstanceId id, unsigned attempt);

    /** On T_BOOT / reset-wait expiry: gpio-mon re-read of BC; if still
     *  asserted, inject → BcHi and return true (skip fault / Timeout). */
    bool tryRecoverStickyBc_(InstanceId id);

    // Redfish path resolution from descriptor.id strings ───────────────────
    static std::string elementRedfishPath_(const config::ElementDescriptor& e);
    static std::string moduleRedfishPath_(const config::ModuleDescriptor& m);
    static std::string fpgaRedfishPath_(const config::FpgaDescriptor& f);
    static std::string trayRedfishPath_(const std::string& chassisId);

    sdbusplus::bus::bus&                  bus_;
    sd_event*                             loop_;

    // Per-descriptor runtime config (copied at init).
    std::chrono::milliseconds             tBoot_{12'000};
    std::string                           platform_;

    // Owning containers — pointers held in lookup tables below.
    std::vector<std::unique_ptr<hal::IDevice>>           devices_;
    std::vector<std::unique_ptr<fsm::ElementFsm>>        elementFsms_;
    std::vector<std::unique_ptr<fsm::ModuleFsm>>         moduleFsms_;
    std::unique_ptr<fsm::TrayFsm>                        trayFsm_;
    std::vector<std::unique_ptr<reporting::Reporter>>    reporters_;

    // Lookup: target instance → ElementFsm / ModuleFsm / Device.
    // Key is packed InstanceId (class | index).
    std::unordered_map<uint64_t, fsm::ElementFsm*>       elementByInstance_;
    std::unordered_map<uint64_t, fsm::ModuleFsm*>        moduleByInstance_;
    std::unordered_map<uint64_t, hal::IDevice*>          deviceByInstance_;

    // Element → parent Module (descriptor tells us which module each
    // LPU/MCU/GPU belongs to).
    std::unordered_map<uint64_t, uint64_t>               elementToModule_;

    // Forward + reverse maps between user-facing descriptor.id strings
    // ("HGX_LPU_3", "HGX_FPGA_0") and packed internal InstanceIds.
    // Populated during initFromDescriptor in walk order; lets the D-Bus
    // layer translate in either direction without re-parsing the
    // descriptor.
    std::unordered_map<std::string, uint64_t>            instanceByDescId_;
    std::unordered_map<uint64_t, std::string>            descIdByInstance_;

    // Boot timers (timerfd-backed). One per Element instance; armed by the
    // Element FSM's startTBoot hook, cancelled by cancelTBoot.
    struct BootTimer
    {
        sd_event_source* source = nullptr;
        InstanceId       owner{};
    };
    std::unordered_map<uint64_t, BootTimer>              bootTimers_;

    struct TelemetryRetryTimer
    {
        sd_event_source* source = nullptr;
        InstanceId owner{};
        unsigned attempt = 0;
    };
    std::unordered_map<uint64_t, TelemetryRetryTimer> telemetryRetryTimers_;

    // In-flight async resets (one per target at a time). Holds:
    //  - deadline: one-shot timer that fires Timeout if BcHi never arrives
    //    via gpio-mon Line PropertiesChanged → emit()
    //  - callback: completion sink (sends D-Bus reply)
    struct PendingReset
    {
        sd_event_source* deadline = nullptr;
        InstanceId       target{};
        std::function<void(ResetOutcome)> callback;
    };
    std::unordered_map<uint64_t, std::unique_ptr<PendingReset>> pendingResets_;

    // Helper: complete + erase the pending reset for `key`. Invokes the
    // callback once; subsequent BcHi events for the same target are
    // routed to the FSM only (no double-completion).
    void completePending_(uint64_t key, ResetOutcome outcome);

    // Helper: install the BC-wait deadline for an already-pulsed `target`.
    // Used by both requestResetAsync (single-
    // element path) and requestResetAllSync (batched mass-reset path).
    // Returns true when the wait timer is armed (callback later); false
    // when timer install fails (callback invoked with PulseFailed).
    bool armResetCompletionWait_(
        InstanceId target,
        hal::IDevice* dev,
        std::function<void(ResetOutcome)> callback);

    // sd-event io sources for each driver fd we've registered.
    std::vector<sd_event_source*>                        ioSources_;

    // Process-wide refcounted GPIO line cache. Injected into every
    // DeviceConfig the orchestrator builds; drivers acquire SharedLines
    // through it instead of opening GPIO chardevs directly. This is what
    // makes shared module-level signals (LP30 EB1 + C00: one MODULE_PGOOD_R
    // for two LPUs) work without EBUSY.
    hal::common::SharedLineRegistry                      sharedLines_;
    hal::common::ResetChipRegistry                       resetChips_;

    // Already-registered fds. Multiple drivers can return the same fd from
    // IDevice::fds() when they share a SharedLine; we register each unique
    // fd once with sd-event and rely on SharedLine::dispatch to fan events
    // out to every subscriber.
    std::unordered_set<int>                              registeredFds_;
};

}  // namespace pwrseq
