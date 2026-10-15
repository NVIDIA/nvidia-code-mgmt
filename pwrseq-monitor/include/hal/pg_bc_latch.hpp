/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * PG edge helper with BC level-latch under PG validity (NVBUG 6559782).
 *
 * The element FSM is edge-driven and only seeds live GPIO levels at daemon
 * start. A BOOT_COMPLETE edge that arrives while the element is still in
 * S_STBY is discarded; if BC is already asserted when PG later rises, no
 * fresh BcHi edge ever comes and the element sticks in BOOT_TIMEOUT until
 * restart.
 *
 * Contract:
 *   - Rising PG: emit PgHi; if BC is already latched asserted, also emit
 *     BcHi (OOO / seed recovery).
 *   - Falling PG: invalidate the BC latch first, then emit PgLo, so the
 *     next PgHi cannot observe a stale BC=1 from the previous power cycle.
 *   - BcLo edges still clear the SharedLine cache via notifyExternal and
 *     drive S_ONLINE → BootRegress in the FSM.
 *   - On T_BOOT / reset-completion expiry without a BcHi edge,
 *     tryRecoverStickyBootComplete() may synthesize BcHi when gpio-mon
 *     still shows BC asserted (sticky FPGA CONF_DONE across N_CONFIG).
 */

#pragma once

#include "core/types.hpp"
#include "hal/i_device.hpp"

#include <optional>
#include <utility>

namespace pwrseq::hal
{

/** Emit a power-good edge with BC level-latch semantics.
 *
 *  @param sink                FSM event sink (Orchestrator / test harness).
 *  @param id                  Element instance.
 *  @param pgAsserted          true = rising PG, false = falling PG.
 *  @param bcAlreadyAsserted   latched BC level (SharedLine hasValue && read).
 *                             Only consulted on rising PG.
 *  @param invalidateBcLatch   called on falling PG before PgLo is emitted
 *                             (typically IDevice::clearBootCompleteCache).
 */
template <typename InvalidateBc>
void emitPowerGoodEdge(SemanticEventSink& sink, InstanceId id, bool pgAsserted,
                       bool bcAlreadyAsserted, InvalidateBc&& invalidateBcLatch)
{
    if (pgAsserted)
    {
        sink.emit(SemanticEvent{EventKind::PgHi, id});
        if (bcAlreadyAsserted)
        {
            sink.emit(SemanticEvent{EventKind::BcHi, id});
        }
        return;
    }

    std::forward<InvalidateBc>(invalidateBcLatch)();
    sink.emit(SemanticEvent{EventKind::PgLo, id});
}

/** True when a gpio-mon electrical ReadGpio level means BC is logically
 *  asserted given descriptor polarity. */
inline bool bootCompleteLogicallyAsserted(bool electrical,
                                          bool activeLow) noexcept
{
    return activeLow ? !electrical : electrical;
}

/** On T_BOOT / reset-completion expiry without a BcHi edge: if gpio-mon
 *  still shows BC asserted (sticky CONF_DONE / BOOT_COMPLETE), inject that
 *  level so subscribers emit BcHi instead of faulting BOOT_TIMEOUT.
 *
 *  @param electrical  raw gpio-mon ReadGpio value (nullopt = unavailable).
 *  @param activeLow   BC LineRef polarity from platform.json.
 *  @param inject      called with the electrical level when recovering;
 *                     must return true if the SharedLine was updated.
 *  @return true if recovery was applied (caller must NOT dispatch
 *          TBootExpired / Timeout). */
template <typename Inject>
bool tryRecoverStickyBootComplete(std::optional<bool> electrical,
                                  bool activeLow, Inject&& inject)
{
    if (!electrical ||
        !bootCompleteLogicallyAsserted(*electrical, activeLow))
    {
        return false;
    }
    return static_cast<bool>(std::forward<Inject>(inject)(*electrical));
}

}  // namespace pwrseq::hal
