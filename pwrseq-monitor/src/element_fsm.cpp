/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Element FSM transition table. Mirrors design doc §2.1.2 one-for-one.
 */

#include "fsm/element_fsm.hpp"

#include <array>

namespace pwrseq::fsm
{

using Row = TransitionRow<ElementState, ElementContext>;

namespace
{

/** The canonical Element FSM transition table.
 *
 *  Returned by a function (Meyer's singleton) rather than declared as a
 *  namespace-scope static, to guarantee initialization-on-first-use ordering.
 *  This avoids static-initialization-order issues when test registration
 *  objects in another translation unit run before this TU's globals would
 *  otherwise be constructed.
 *
 *  Mirrors design doc §2.1.2 one row per cell.
 */
std::span<const Row> table()
{
    // Action helpers — function-local statics, captureless lambdas wrapped
    // in std::function. All initialized before the first table() call returns.
    static const auto startTBoot = [](ElementContext& ctx,
                                      const SemanticEvent&) {
        if (ctx.startTBoot)
            ctx.startTBoot();
    };

    static const auto cancelTBoot = [](ElementContext& ctx,
                                       const SemanticEvent&) {
        if (ctx.cancelTBoot)
            ctx.cancelTBoot();
    };

    static const auto setFaultBootTimeout = [](ElementContext& ctx,
                                               const SemanticEvent&) {
        ctx.faultCode = FaultCode::BootTimeout;
    };

    static const auto setFaultBootRegress = [](ElementContext& ctx,
                                               const SemanticEvent&) {
        ctx.faultCode = FaultCode::BootRegress;
    };

    static const auto setFaultPgoodDrop = [](ElementContext& ctx,
                                             const SemanticEvent&) {
        ctx.faultCode = FaultCode::PgoodDrop;
    };

    static const auto clearFault = [](ElementContext& ctx,
                                      const SemanticEvent&) {
        ctx.faultCode = FaultCode::None;
    };

    // PG re-asserted after the element faulted. Must restart the boot path:
    // clear the fault and re-arm T_BOOT. Crucially, we re-enter S_POWERED
    // rather than jumping back to S_ONLINE, because after a power cycle the
    // LPU has actually rebooted and needs to re-validate boot completion
    // within T_BOOT. Letting it leap straight to S_ONLINE on the subsequent
    // BcHi (the pre-fix behavior) hid second-cycle boot failures: a hung
    // re-boot would never trip T_BOOT because the timer was never re-armed.
    static const auto clearFaultAndStartTBoot = [](ElementContext& ctx,
                                                   const SemanticEvent&) {
        ctx.faultCode = FaultCode::None;
        if (ctx.startTBoot)
            ctx.startTBoot();
    };

    // Guard for the BC-driven recovery row (S_FAULT + EV_BcHi -> S_ONLINE).
    // Two operationally important cases qualify:
    //
    //   * BootTimeout — "T_BOOT expired but the MCU was still booting; BC
    //     finally arrived, so the boot succeeded". The LPU never lost
    //     power, the late BC is the first time boot-complete was seen,
    //     and treating it as recovery is correct.
    //
    //   * BootRegress — "MCU dropped BC while we were S_ONLINE, then
    //     re-asserted BC". On LP30 the BC line is a per-LPU MCU output
    //     (LPU<N>_MCU_C_OK-I), electrically clean (not a noisy bus), so a
    //     1->0->1 sequence in practice means the MCU watchdog'd and came
    //     back. Auto-recovering keeps Redfish Status.State accurate
    //     through transient MCU hiccups rather than parking the element
    //     until an operator intervenes — operator visibility is preserved
    //     by the journal showing the fault entry and the explicit
    //     `S_FAULT -> S_ONLINE` recovery transition.
    //
    // PgoodDrop is deliberately NOT in the guard: that fault means power
    // actually went away, and a bare BcHi without a fresh PgHi cycle
    // would reflect a stale latched BC value rather than a real recovery.
    // PgoodDrop recovery still requires the full S_FAULT -> S_POWERED
    // -> S_ONLINE walk via EV_PgHi.
    static const auto guardLateBootRecovery = [](const ElementContext& ctx,
                                                 const SemanticEvent&) -> bool {
        return ctx.faultCode == FaultCode::BootTimeout ||
               ctx.faultCode == FaultCode::BootRegress;
    };

    // S_STBY anomaly: a BC event arrived without PG. The FSM keeps the
    // edge as a no-op here; LpuDriver/FpgaDriver latch the BC level in
    // SharedLine and synthesize BcHi on the subsequent PgHi when the
    // latch is still asserted (hal/pg_bc_latch.hpp — NVBUG 6559782).
    static const auto logBcAnomaly = [](ElementContext&, const SemanticEvent&) {
    };

    static const std::array<Row, 17> kTable = {{
        // ---- from S_STBY ----
        {ElementState::Stby, EventKind::PgHi, nullptr, ElementState::Powered,
         startTBoot},
        {ElementState::Stby, EventKind::PgLo, nullptr, ElementState::Stby,
         nullptr},
        {ElementState::Stby, EventKind::BcHi, nullptr, ElementState::Stby,
         logBcAnomaly},
        {ElementState::Stby, EventKind::BcLo, nullptr, ElementState::Stby,
         logBcAnomaly},

        // ---- from S_POWERED ----
        {ElementState::Powered, EventKind::BcHi, nullptr, ElementState::Online,
         cancelTBoot},
        {ElementState::Powered, EventKind::PgLo, nullptr, ElementState::Stby,
         cancelTBoot},
        {ElementState::Powered, EventKind::TBootExpired, nullptr,
         ElementState::Fault, setFaultBootTimeout},
        {ElementState::Powered, EventKind::RstAsserted, nullptr,
         ElementState::Resetting, cancelTBoot},

        // ---- from S_ONLINE ----
        {ElementState::Online, EventKind::BcLo, nullptr, ElementState::Fault,
         setFaultBootRegress},
        {ElementState::Online, EventKind::PgLo, nullptr, ElementState::Fault,
         setFaultPgoodDrop},
        {ElementState::Online, EventKind::RstAsserted, nullptr,
         ElementState::Resetting, nullptr},

        // ---- from S_RESETTING ----
        {ElementState::Resetting, EventKind::RstReleased, nullptr,
         ElementState::Powered, startTBoot},
        {ElementState::Resetting, EventKind::PgLo, nullptr, ElementState::Stby,
         nullptr},

        // ---- from S_FAULT ----
        // Power re-asserted after a fault: re-enter the boot path and
        // re-arm T_BOOT. Covers two operationally important cases:
        //   * PgoodDrop recovery -- LPU power-cycled, must re-validate boot
        //     within T_BOOT (without this row, the daemon never re-arms the
        //     boot deadline timer and a hung second boot is missed).
        //   * Spurious PgHi while in S_FAULT(BootTimeout) -- harmless;
        //     re-arming T_BOOT is the conservative default. If PG truly
        //     never dropped, the daemon should not emit a fresh PgHi edge.
        {ElementState::Fault, EventKind::PgHi, nullptr, ElementState::Powered,
         clearFaultAndStartTBoot},
        // Late-boot recovery: BC asserts after T_BOOT expired → S_ONLINE.
        // Guarded to fire ONLY on BootTimeout faults so that PgoodDrop /
        // BootRegress recoveries are forced through the full boot path
        // (S_FAULT → S_POWERED via PgHi → S_ONLINE via BcHi) rather than
        // shortcutting on a possibly stale BC level.
        {ElementState::Fault, EventKind::BcHi, guardLateBootRecovery,
         ElementState::Online, clearFault},
        // Power-down clears the fault.
        {ElementState::Fault, EventKind::PgLo, nullptr, ElementState::Stby,
         clearFault},
        // Redfish-initiated reset on a faulted element — clear the fault
        // and start a new reset cycle (T_BOOT will re-arm on RstReleased).
        {ElementState::Fault, EventKind::RstAsserted, nullptr,
         ElementState::Resetting, clearFault},
    }};

    return std::span<const Row>(kTable);

    // Note: the design doc §2.1.2 row "S_FAULT EV_RST_RELEASED -> S_RESETTING
    // (engine-policy retry)" is no longer implemented — the daemon does
    // one-shot resets only (a Redfish reset request dispatches a fresh
    // EV_RST_ASSERTED which naturally goes S_FAULT → S_RESETTING via the
    // standard reset path; we just never inject that automatically).
}

} // namespace

ElementFsm::ElementFsm(InstanceId id) :
    ctx_{.id = id,
         .faultCode = FaultCode::None,
         .startTBoot = {},
         .cancelTBoot = {},
         .onStateChange = {}},
    engine_(table(), ElementState::Stby)
{}

bool ElementFsm::dispatch(const SemanticEvent& ev)
{
    const ElementState prev = engine_.currentState();
    const bool fired = engine_.dispatch(ctx_, ev);
    if (fired)
    {
        if (ev.kind == EventKind::PgHi)
        {
            seenPgHi_ = true;
        }
        const ElementState now = engine_.currentState();
        if (now != prev && ctx_.onStateChange)
        {
            ctx_.onStateChange(prev, now);
        }
    }
    return fired;
}

std::span<const TransitionRow<ElementState, ElementContext>> elementFsmTable()
{
    return table();
}

} // namespace pwrseq::fsm
