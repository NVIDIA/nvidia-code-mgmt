/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "core/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pwrseq::hal
{

namespace common
{
class ResetChipRegistry;
class SharedLineRegistry;
} // namespace common

/** Return code for HAL operations. */
enum class Result : int
{
    Ok = 0,
    NotPresent, // probe() — device isn't here
    Absent,     // init() -- GPIO chip not found; module not populated
    AlreadyInitialized,
    InitFailed,
    NotReady, // transient state; caller may retry the operation
    HwError,
    BadArgument,
    NotSupported, // capability not advertised
};

std::string_view to_string(Result r) noexcept;

/** Kind of reset to perform. */
enum class ResetKind : uint8_t
{
    Warm,       // LSTP reset trigger (assert only; MCU clears the net)
    Cold,       // hard power off + on (RESET_3V3 on FPGA, etc.)
    PowerCycle, // alias for Cold in most cases
};

/** Capabilities a HAL driver advertises to the FSM Engine.
 *  Used to skip transitions that don't apply (e.g. no S_RESETTING state for
 *  a device that doesn't support warm reset).
 */
struct Capabilities
{
    bool powerGood = false;      // EV_PG_HI/LO available
    bool bootComplete = false;   // EV_BC_HI/LO available
    bool warmReset = false;      // reset(Warm) supported
    bool coldReset = false;      // reset(Cold) supported
    bool powerCycle = false;     // reset(PowerCycle) supported
    bool perRailHealth = false;  // multiple per-rail PGs aggregated
    bool presenceDetect = false; // PRSNT line readable
    bool pmbusAlert = false;     // PMB_ALERT line readable
};

/** Snapshot of a device's current observable state. Read at daemon startup
 *  to seed the FSM, and on demand for Redfish queries.
 */
struct DeviceStatus
{
    bool present = false;
    bool powerGood = false;
    bool bootComplete = false;
    // Raw boot-state register value (LPU/FPGA), 0xFF if not readable.
    uint8_t bootStateRegister = 0xFF;
};

/** Per-instance configuration handed to IDevice::init().
 *  Most fields come from the platform descriptor (§6 of the design).
 */
struct DeviceConfig
{
    InstanceId instanceId;
    std::chrono::milliseconds initTimeoutMs{12'000}; // T_BOOT default 12s

    /** Driver-specific signal-map / line-name strings. Format is up to the
     *  driver (e.g. an LpuDriver expects keys like "pg_line", "bc_line",
     *  "rst_line"). The orchestrator populates this from the descriptor.
     */
    std::vector<std::pair<std::string, std::string>> driverParams = {};

    /** Process-wide registry the driver should use to acquire GPIO lines.
     *  Required for production wiring — the orchestrator owns one and
     *  injects a non-null pointer here. Tests may leave it null when the
     *  unit under test does not need real GPIO (the driver returns
     *  Result::BadArgument from init() in that case).
     *
     *  Lifetime: must outlive any IDevice that init()-s with this config.
     */
    common::SharedLineRegistry* sharedLines = nullptr;

    /** Process-wide reset-line gpiochip cache. Required for production
     *  wiring — the orchestrator owns one and injects a non-null pointer.
     *  Lifetime: must outlive any IDevice that init()-s with this config. */
    common::ResetChipRegistry* resetChips = nullptr;

    /** Optional lookup for a parameter by key. Empty string if absent. */
    std::string param(std::string_view key) const noexcept;
};

/** Sink for semantic events emitted by a HAL driver.
 *  The driver calls emit() whenever it observes a hardware edge or telemetry
 *  change worth notifying the FSM. emit() is called from the daemon's main
 *  event-loop thread (we are single-threaded — see §7.1).
 */
class SemanticEventSink
{
  public:
    virtual ~SemanticEventSink() = default;
    virtual void emit(const SemanticEvent& ev) = 0;
};

/** HAL device interface. Every device class (LPU, FPGA, BB, ERoT, …)
 *  implements this. The FSM Engine never holds a reference to a concrete
 *  driver — it only ever sees SemanticEvents emitted upward and reset
 *  commands dispatched downward through this interface.
 */
class IDevice
{
  public:
    virtual ~IDevice() = default;

    // ── lifecycle ──────────────────────────────────────────────────────────
    /** Test for device presence (PRSNT line, USB enumeration, etc.).
     *  Called before init(). Returns Ok if device should be wired up;
     *  NotPresent if the slot is empty (orchestrator skips init).
     */
    virtual Result probe() = 0;

    /** Open hardware resources (reset output lines, NSM client, …)
     *  and prepare to emit events to `sink`. Should not block; expected to
     *  return quickly. The driver MAY also call sink->emit() during init()
     *  with the initial state if useful, but typically does not — the
     *  Gpio.Monitor.Line property subscriber seeds via injectLineState.
     */
    virtual Result init(SemanticEventSink& sink, const DeviceConfig& cfg) = 0;

    /** Drive a reset on the device. One-shot: the driver triggers the reset
     *  line (assert only on LSTP/USB platforms) and returns immediately.
     *  The module MCU clears the reset net; completion is observed via BC.
     *  No retry — the Element FSM transitions to S_FAULT(BOOT_TIMEOUT) if
     *  the device fails to come back online within T_BOOT.
     */
    virtual Result reset(ResetKind kind) = 0;

    /** Batched mass-reset helpers.
     *
     *  resetAssert(kind) drives the reset line to its asserted (active) state
     *  and returns immediately. On LP30 LSTP lines that is the full hardware
     *  actuation — the remote MCU owns hold/release timing. resetRelease() is
     *  not invoked on the warm-reset path; RstReleased is dispatched in
     *  software once all asserts succeed.
     *
     *  Devices that don't model a per-element reset line (or that delegate
     *  reset to a systemd helper unit) should leave the defaults and
     *  return NotSupported; the orchestrator falls back to per-element
     *  `reset()` calls in that case.
     */
    virtual Result resetAssert(ResetKind /*kind*/)
    {
        return Result::NotSupported;
    }
    virtual Result resetRelease()
    {
        return Result::NotSupported;
    }

    /** Release hardware resources cleanly. Called at daemon shutdown. */
    virtual Result shutdown() = 0;

    /** Drop cached boot-complete before a reset pulse so async reset
     *  completion waits for a fresh BcHi edge, not a pre-reset level. */
    virtual void clearBootCompleteCache()
    {}

    /** gpio-mon LineName for boot-complete (empty if the device has none). */
    virtual std::string_view bootCompleteLineName() const noexcept
    {
        return {};
    }

    /** Descriptor polarity for bootCompleteLineName(). */
    virtual bool bootCompleteActiveLow() const noexcept
    {
        return false;
    }

    /** Inject a raw electrical gpio-mon level into the BC SharedLine
     *  (applies polarity, notifies subscribers → BcHi/BcLo).
     *  @return true if a SharedLine was updated. */
    virtual bool injectBootCompleteElectrical(bool /*electricalAsserted*/)
    {
        return false;
    }
    // ── inspection ─────────────────────────────────────────────────────────
    virtual DeviceStatus get_status() const = 0;
    virtual Capabilities capabilities() const = 0;
    virtual InstanceId id() const = 0;

    // ── event-loop integration ─────────────────────────────────────────────
    /** File descriptors the driver wants polled for input. PG/BC drivers
     *  return an empty vector (gpio-mon dispatches edges via D-Bus).
     */
    virtual std::vector<int> fds() const = 0;

    /** Called when a driver-supplied fd becomes readable. Unused for
     *  PG/BC on LP30 (edges arrive via Gpio.Monitor.Line PropertiesChanged).
     */
    virtual void onReadable(int fd) = 0;

    /** Enable or disable lpu850 kernel polling (telemetry + log drain) for
     *  this element. Default NotSupported — only LpuDriver implements it
     *  when a log_dev chardev is configured. Called on the Online FSM
     *  boundary by the orchestrator. */
    virtual Result setTelemetryPolling(bool /*enable*/)
    {
        return Result::NotSupported;
    }

    /** Record whether platform configuration delegates VR telemetry to this
     *  LPU. The LpuDriver combines this request with Online state and firmware
     *  capability before setting the kernel VR poll bit. */
    virtual Result setVrTelemetryConfigured(bool /*enable*/)
    {
        return Result::NotSupported;
    }

    /** Request an event-driven LPU log drain (fault/thermal/PMBus). */
    virtual Result requestLogDrain(std::string_view /*reason*/)
    {
        return Result::NotSupported;
    }
};

} // namespace pwrseq::hal
