/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tier A test rig — wires the production FSM hierarchy
 * (ElementFSM × N + ModuleFSM × M + TrayFSM × 1) from a synthetic
 * PlatformDescriptor, with MockDevice instances in place of the real HAL.
 *
 * See test/TEST_PLAN.md for the design rationale. The short version:
 * FSM inputs are not BMC-writable on real hardware, so the canonical way
 * to test the FSMs is to inject SemanticEvents through the same sink
 * abstraction the real HAL uses. This rig is that injection point.
 */

#pragma once

#include "config/descriptor.hpp"
#include "core/types.hpp"
#include "fsm/element_fsm.hpp"
#include "fsm/module_fsm.hpp"
#include "fsm/tray_fsm.hpp"
#include "hal/i_device.hpp"
#include "hal/mock_device.hpp"
#include "virtual_clock.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pwrseq::test
{

/** One record in the rig's transition trace.
 *
 *  The rig records every state change at every tier so scenarios can
 *  assert ordering (e.g. element transitions before its parent module
 *  transitions).
 */
struct TransitionRecord
{
    enum class Tier : uint8_t
    {
        Element,
        Module,
        Tray,
    };

    Tier tier;
    InstanceId who;
    int fromState; // numeric value of the enum
    int toState;
    std::chrono::milliseconds at; // VirtualClock::now() at the transition
};

/** Tier A test rig. Builds the FSM hierarchy from a PlatformDescriptor,
 *  exposes inject() / tick() / requestReset() for stimulus, and
 *  elementState() / moduleState() / trayState() for observation.
 *
 *  Lifetimes: the rig owns every FSM, every MockDevice, and the
 *  VirtualClock. Tear it down by letting it go out of scope.
 */
class TestRig : public hal::SemanticEventSink
{
  public:
    /** Construct a rig with the topology described by `desc`. Allocates
     *  one MockDevice per element + per FPGA, one ElementFsm per device,
     *  one ModuleFsm per module, and one TrayFsm. Hooks up all
     *  upward-propagation callbacks. Does NOT seed any initial events —
     *  the rig starts with every FSM in its respective Stby state.
     */
    explicit TestRig(const config::PlatformDescriptor& desc);

    ~TestRig() override = default;

    TestRig(const TestRig&) = delete;
    TestRig& operator=(const TestRig&) = delete;

    // ── stimulus ────────────────────────────────────────────────────────

    /** Inject a hardware-edge event on a specific element. Equivalent to
     *  a HAL driver calling sink.emit() in production.
     *
     *  Valid event kinds: PgHi, PgLo, BcHi, BcLo, RstAsserted, RstReleased,
     *  TBootExpired. Aggregator events (LpuStateChanged, FpgaStateChanged,
     *  ModuleStateChanged) are dispatched internally by the rig and should
     *  not be injected by tests.
     */
    void inject(InstanceId target, EventKind kind);

    /** Inject the same event on every LPU. Useful for cold-boot scenarios. */
    void injectAllLpus(EventKind kind);

    /** Inject the same event on every FPGA. */
    void injectAllFpgas(EventKind kind);

    /** Advance the virtual clock. Any T_BOOT timer whose deadline is now
     *  crossed fires (it dispatches `EV_T_BOOT_EXPIRED` to its owning
     *  element). Returns the number of timers that fired.
     */
    int tick(std::chrono::milliseconds delta);

    /** Simulate a Redfish-initiated reset on `target`. Equivalent to
     *  Orchestrator::requestReset(): dispatches `EV_RST_ASSERTED`, then
     *  invokes the underlying MockDevice::reset() (which only records the
     *  call), then dispatches `EV_RST_RELEASED`.
     *
     *  The rig does NOT auto-emit BC events after a reset — that's the
     *  scenario's responsibility, since real LPUs take seconds to reboot
     *  and the test wants to control timing explicitly.
     */
    void requestReset(InstanceId target,
                      hal::ResetKind kind = hal::ResetKind::Warm);

    // ── observation ─────────────────────────────────────────────────────

    /** Current state of an element FSM (LPU or FPGA). */
    ElementState elementState(InstanceId id) const;

    /** Current state of a module FSM. */
    ModuleState moduleState(InstanceId id) const;

    /** Current state of the tray FSM. */
    TrayState trayState() const;

    /** Last-set fault code on an element FSM. Meaningful only when
     *  elementState(id) == Fault.
     */
    FaultCode elementFault(InstanceId id) const;

    /** Number of times reset() was called on the device backing `id`. */
    int resetCallCount(InstanceId id) const;

    /** Reset-kind history for the device backing `id`. */
    const std::vector<hal::ResetKind>& resetHistory(InstanceId id) const;

    /** Full transition trace, oldest first. Cleared by clearTrace(). */
    const std::vector<TransitionRecord>& trace() const noexcept
    {
        return trace_;
    }

    void clearTrace() noexcept
    {
        trace_.clear();
    }

    /** Current virtual time. */
    std::chrono::milliseconds now() const noexcept
    {
        return clock_.now();
    }

    /** Number of T_BOOT timers currently armed across all elements. */
    size_t pendingTimers() const noexcept
    {
        return clock_.pendingCount();
    }

    // ── lookup helpers (all zero-based, matching production) ────────────
    //
    // InstanceId::index is zero-based in production (see
    // include/core/types.hpp and Orchestrator::initFromDescriptor's
    // `nextElementIdx = 0`). The harness now mirrors that exactly so
    // test InstanceIds match what production allocates from the same
    // descriptor. NOTE: tests that previously used 1-based LPU indices
    // need to switch to zero-based.

    /** InstanceId for the i-th LPU (0..15). */
    static constexpr InstanceId lpuId(uint16_t i) noexcept
    {
        return {InstanceId::Class::Lpu, i};
    }

    /** InstanceId for the i-th FPGA (0..1). */
    static constexpr InstanceId fpgaId(uint16_t i) noexcept
    {
        return {InstanceId::Class::Fpga, i};
    }

    /** InstanceId for the i-th module (0..7). */
    static constexpr InstanceId moduleId(uint16_t i) noexcept
    {
        return {InstanceId::Class::Module, i};
    }

    /** InstanceId for the tray. */
    static constexpr InstanceId trayId() noexcept
    {
        return {InstanceId::Class::Tray, 0};
    }

    // ── SemanticEventSink (events from MockDevice land here) ────────────

    void emit(const SemanticEvent& ev) override;

  private:
    // Wire one element FSM into the hierarchy. `parentModule` is empty
    // for FPGAs (their state changes propagate directly to the Tray).
    void wireElement_(fsm::ElementFsm& fsm,
                      std::optional<InstanceId> parentModule);

    // Wire one module FSM into the hierarchy.
    void wireModule_(fsm::ModuleFsm& fsm);

    // Wire the tray FSM (state changes are only recorded in the trace).
    void wireTray_();

    // Dispatch a SemanticEvent to the right Element / Module / Tray FSM.
    void dispatchInternal_(const SemanticEvent& ev);

    // Lookup helpers — return nullptr if not found.
    fsm::ElementFsm* findElement_(InstanceId id) const;
    fsm::ModuleFsm* findModule_(InstanceId id) const;
    hal::test::MockDevice* findDevice_(InstanceId id) const;

    // Returns true iff `id` corresponds to an FSM the rig owns. Used by
    // inject() / requestReset() to reject typos loudly instead of
    // silently no-op'ing.
    bool targetExists_(InstanceId id) const;

    // Packed-key form of InstanceId — same trick the orchestrator uses
    // for its lookup tables.
    static uint64_t key_(InstanceId id) noexcept
    {
        return (uint64_t)id.cls << 32 | id.index;
    }

    // ────────────────────────────────────────────────────────────────────

    VirtualClock clock_;
    std::chrono::milliseconds tBoot_;

    std::vector<std::unique_ptr<hal::test::MockDevice>> devices_;
    std::vector<std::unique_ptr<fsm::ElementFsm>> elements_;
    std::vector<std::unique_ptr<fsm::ModuleFsm>> modules_;
    std::unique_ptr<fsm::TrayFsm> tray_;

    std::unordered_map<uint64_t, fsm::ElementFsm*> elementByInstance_;
    std::unordered_map<uint64_t, fsm::ModuleFsm*> moduleByInstance_;
    std::unordered_map<uint64_t, hal::test::MockDevice*> deviceByInstance_;

    // Element → parent Module (FPGAs have no entry; their parent is the Tray).
    std::unordered_map<uint64_t, uint64_t> elementToModule_;

    // Tier of each element (for state-changed event routing): true if LPU
    // (state changes go up to its Module), false if FPGA (state changes go
    // directly to the Tray).
    std::unordered_map<uint64_t, bool> elementIsLpu_;

    // For Module/FPGA index lookup when emitting aggregator events with
    // payload (the Tray FSM indexes into moduleStates[] / fpgaStates[]
    // by InstanceId::index — which we already use 0-based for both).

    // Active T_BOOT timer per element (id → VirtualClock TimerId).
    // Zero means "no timer armed".
    std::unordered_map<uint64_t, VirtualClock::TimerId> bootTimers_;

    // Transition trace (one entry per recorded state change).
    std::vector<TransitionRecord> trace_;
};

} // namespace pwrseq::test
