/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Element FSM unit tests. Covers every row in the design-doc §2.1.2
 * transition matrix plus end-to-end scenarios (cold boot, late-boot
 * recovery, runtime reset, fault paths).
 *
 * Hardware-free: timer hook records calls instead of arming a real timer;
 * onStateChange hook records the transition trace.
 */

#include "fsm/element_fsm.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace pwrseq;
using namespace pwrseq::fsm;

namespace
{

constexpr InstanceId kLpu1{InstanceId::Class::Lpu, 1};

/** Test harness wrapping an ElementFsm + recording its side-effect hooks. */
struct Harness
{
    ElementFsm fsm{kLpu1};

    int startTBootCalls  = 0;
    int cancelTBootCalls = 0;
    std::vector<std::pair<ElementState, ElementState>> transitions;

    Harness()
    {
        fsm.setStartTBoot ([this]() { ++startTBootCalls; });
        fsm.setCancelTBoot([this]() { ++cancelTBootCalls; });
        fsm.setOnStateChange(
            [this](ElementState a, ElementState b) {
                transitions.emplace_back(a, b);
            });
    }

    void send(EventKind k)
    {
        fsm.dispatch(SemanticEvent{k, kLpu1});
    }
};

}  // namespace

// ─── per-row tests (one per row in design-doc §2.1.2) ───────────────────────

TEST(ElementFsm, InitialStateIsStby)
{
    Harness h;
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
}

TEST(ElementFsm, Stby_PgHi_GoesToPoweredAndStartsTBoot)
{
    Harness h;
    h.send(EventKind::PgHi);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.cancelTBootCalls, 0);
}

TEST(ElementFsm, Stby_PgLo_StaysStby)
{
    Harness h;
    h.send(EventKind::PgLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_FALSE(h.fsm.hasSeenPgHi());
    EXPECT_TRUE(h.transitions.empty());
}

TEST(ElementFsm, PgHi_SetsSeenPgHiLatch)
{
    Harness h;
    EXPECT_FALSE(h.fsm.hasSeenPgHi());
    h.send(EventKind::PgHi);
    EXPECT_TRUE(h.fsm.hasSeenPgHi());
    h.send(EventKind::PgLo);
    EXPECT_TRUE(h.fsm.hasSeenPgHi());
}

TEST(ElementFsm, Stby_BcEvents_StayStbyAndLogAnomaly)
{
    Harness h;
    h.send(EventKind::BcHi);  // anomaly: BC without PG
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    h.send(EventKind::BcLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_TRUE(h.transitions.empty());  // self-transitions don't notify
}

TEST(ElementFsm, Powered_BcHi_GoesOnlineAndCancelsTBoot)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.cancelTBootCalls, 1);
}

TEST(ElementFsm, Powered_PgLo_GoesStbyAndCancelsTBoot)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::PgLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_EQ(h.cancelTBootCalls, 1);
}

TEST(ElementFsm, Powered_TBootExpired_GoesFaultWithBootTimeout)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::TBootExpired);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
}

TEST(ElementFsm, Powered_RstAsserted_GoesResettingAndCancelsTBoot)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::RstAsserted);
    EXPECT_EQ(h.fsm.state(), ElementState::Resetting);
    EXPECT_EQ(h.cancelTBootCalls, 1);
}

TEST(ElementFsm, Online_BcLo_GoesFaultWithBootRegress)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::BcLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootRegress);
}

TEST(ElementFsm, Online_PgLo_GoesFaultWithPgoodDrop)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::PgLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);
}

TEST(ElementFsm, Online_RstAsserted_GoesResetting)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::RstAsserted);
    EXPECT_EQ(h.fsm.state(), ElementState::Resetting);
}

TEST(ElementFsm, Resetting_RstReleased_GoesPoweredAndRestartsTBoot)
{
    Harness h;
    h.send(EventKind::PgHi);    // → Powered, startTBoot=1
    h.send(EventKind::BcHi);    // → Online, cancelTBoot=1
    h.send(EventKind::RstAsserted);  // → Resetting
    h.send(EventKind::RstReleased);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    EXPECT_EQ(h.startTBootCalls, 2);   // restarted after reset release
}

TEST(ElementFsm, Resetting_PgLo_GoesStby)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::RstAsserted);
    h.send(EventKind::PgLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
}

TEST(ElementFsm, Fault_BcHi_LateBootRecoveryGoesOnlineAndClearsFault)
{
    // Late-boot recovery only applies to BootTimeout faults (LPU never lost
    // power, BC just arrived late). Guarded in element_fsm.cpp.
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::TBootExpired);  // → Fault (BootTimeout)
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
    h.send(EventKind::BcHi);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
}

TEST(ElementFsm, Fault_BcHi_FromPgoodDrop_DoesNotShortcutToOnline)
{
    // Critical recovery-correctness test: after a power cycle, a bare BcHi
    // must NOT bring the element back to S_ONLINE. The LPU has actually
    // rebooted and its BC level may be stale; the daemon must require a
    // fresh boot path (S_FAULT → S_POWERED via PgHi → S_ONLINE via BcHi)
    // so T_BOOT is re-armed and a hung second boot is detected.
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);                       // → Online
    ASSERT_EQ(h.fsm.state(), ElementState::Online);
    h.send(EventKind::PgLo);                       // → Fault (PgoodDrop)
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);

    h.send(EventKind::BcHi);                       // must be ignored
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);
}

TEST(ElementFsm, Fault_BcHi_FromBootRegress_RecoversOnline)
{
    // BootRegress is intentionally in the late-recovery guard: on LP30 a
    // clean MCU C_OK 1→0→1 means the MCU came back; bare BcHi returns
    // S_ONLINE without requiring a PG cycle (unlike PgoodDrop).
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);                       // → Online
    h.send(EventKind::BcLo);                       // → Fault (BootRegress)
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::BootRegress);

    h.send(EventKind::BcHi);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
}

TEST(ElementFsm, Fault_PgHi_FromPgoodDrop_GoesPoweredAndRestartsTBoot)
{
    // The PgoodDrop recovery path: PG returns → S_POWERED with T_BOOT
    // re-armed. The fault is cleared on this transition.
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::PgLo);                       // → Fault (PgoodDrop)
    ASSERT_EQ(h.fsm.state(), ElementState::Fault);
    int startsBefore = h.startTBootCalls;

    h.send(EventKind::PgHi);                       // recovery edge
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
    EXPECT_EQ(h.startTBootCalls, startsBefore + 1);
}

TEST(ElementFsm, Fault_PgLo_GoesStbyAndClearsFault)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::TBootExpired);
    h.send(EventKind::PgLo);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
}

TEST(ElementFsm, Fault_RstAsserted_GoesResettingAndClearsFault)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::TBootExpired);             // → Fault (BootTimeout)
    ASSERT_EQ(h.fsm.state(), ElementState::Fault);
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
    h.send(EventKind::RstAsserted);              // Redfish reset on faulted
    EXPECT_EQ(h.fsm.state(), ElementState::Resetting);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
    h.send(EventKind::RstReleased);              // back to Powered with T_BOOT
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
}

// ─── end-to-end scenarios ───────────────────────────────────────────────────

TEST(ElementFsm, ColdBoot_HappyPath_StbyToOnlineWithOneStartOneCancel)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.cancelTBootCalls, 1);
    EXPECT_EQ(h.transitions.size(), 2u);  // Stby→Powered, Powered→Online
}

TEST(ElementFsm, ColdBoot_BcAfterTimeout_LateBootRecoveryStillEndsOnline)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::TBootExpired);   // → Fault
    h.send(EventKind::BcHi);           // → Online (late-boot recovery)
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::None);
}

TEST(ElementFsm, PowerCycle_FullRoundTrip_ReWalksPoweredAndReArmsTBoot)
{
    // End-to-end power cycle: validates the whole fix as a black box.
    // Cold-boot Stby→Powered→Online, then PG drop+rise+BC must re-walk
    // Powered (re-arming T_BOOT) before reaching Online again.
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);                        // Online
    int startsBeforeCycle = h.startTBootCalls;

    h.send(EventKind::PgLo);                        // → Fault (PgoodDrop)
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);

    h.send(EventKind::PgHi);                        // → Powered, T_BOOT armed
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    EXPECT_EQ(h.startTBootCalls, startsBeforeCycle + 1);

    h.send(EventKind::BcHi);                        // → Online
    EXPECT_EQ(h.fsm.state(), ElementState::Online);

    // Three trace entries from the cycle alone:
    //   Online→Fault, Fault→Powered, Powered→Online.
    auto cycleTrace = h.transitions;
    ASSERT_GE(cycleTrace.size(), 5u);
    EXPECT_EQ(cycleTrace[cycleTrace.size() - 3],
              std::make_pair(ElementState::Online,  ElementState::Fault));
    EXPECT_EQ(cycleTrace[cycleTrace.size() - 2],
              std::make_pair(ElementState::Fault,   ElementState::Powered));
    EXPECT_EQ(cycleTrace[cycleTrace.size() - 1],
              std::make_pair(ElementState::Powered, ElementState::Online));
}

TEST(ElementFsm, PowerCycle_HungSecondBoot_FaultsViaTBootExpired)
{
    // The reason the fix matters: if the second boot hangs, T_BOOT must
    // fire and the element must end up back in S_FAULT with BootTimeout.
    // Before the fix, S_FAULT(PgoodDrop) → BcHi → S_ONLINE was hit before
    // any timer was armed, so this scenario silently passed as healthy.
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);                        // Online
    h.send(EventKind::PgLo);                        // → Fault (PgoodDrop)
    h.send(EventKind::PgHi);                        // → Powered, T_BOOT armed
    ASSERT_EQ(h.fsm.state(), ElementState::Powered);

    // Second boot hangs: timer expires with no BC.
    h.send(EventKind::TBootExpired);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
}

TEST(ElementFsm, RuntimeReset_RoundTrip_BackToOnlineWithFreshTimer)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);             // Online
    int startsBefore = h.startTBootCalls;
    h.send(EventKind::RstAsserted);      // Resetting
    h.send(EventKind::RstReleased);      // Powered (timer restarted)
    EXPECT_EQ(h.startTBootCalls, startsBefore + 1);
    h.send(EventKind::BcHi);             // Online again
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(ElementFsm, ResetThenTimeout_LandsInFault)
{
    Harness h;
    h.send(EventKind::PgHi);
    h.send(EventKind::BcHi);
    h.send(EventKind::RstAsserted);
    h.send(EventKind::RstReleased);     // Powered with restarted T_BOOT
    h.send(EventKind::TBootExpired);    // Fault
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
}

// ─── table sanity ────────────────────────────────────────────────────────────

TEST(ElementFsmTable, EveryRowHasFromAndTo)
{
    auto rows = elementFsmTable();
    EXPECT_GT(rows.size(), 0u);
    for (const auto& row : rows)
    {
        // States must be in-range — sanity check the enum
        EXPECT_LE(static_cast<int>(row.from),
                  static_cast<int>(ElementState::Fault));
        EXPECT_LE(static_cast<int>(row.to),
                  static_cast<int>(ElementState::Fault));
    }
}

TEST(ElementFsmTable, NoDuplicateFromEventPairs)
{
    // FsmEngine::dispatch is first-match-wins, so a duplicate (from, event)
    // is only safe when EVERY row in the duplicate set carries a guard AND
    // the EARLIEST row in walk order also carries a guard (an unguarded
    // earlier row would unconditionally shadow any later guarded one).
    // Two guarded rows can still overlap in practice -- this test catches
    // the easy structural cases; semantic overlap of guards is the row
    // author's responsibility.
    auto rows = elementFsmTable();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        for (size_t j = i + 1; j < rows.size(); ++j)
        {
            const bool sameFromEvent = (rows[i].from == rows[j].from)
                                    && (rows[i].event == rows[j].event);
            if (!sameFromEvent) continue;

            // Earlier row (i) must be guarded -- otherwise it shadows j
            // unconditionally and j is dead code.
            EXPECT_TRUE(rows[i].guard != nullptr)
                << "Duplicate (from, event) pair at rows " << i
                << " (earlier) and " << j << " (later): earlier row is "
                << "unguarded and will unconditionally shadow the later "
                << "row under FsmEngine's first-match-wins dispatch.";

            // Later row (j) must also be guarded -- two unguarded rows
            // would simply be the same row twice.
            EXPECT_TRUE(rows[j].guard != nullptr)
                << "Duplicate (from, event) pair at rows " << i << " and "
                << j << ": later row is unguarded. Either the rows are "
                << "redundant or the later one needs a guard.";
        }
    }
}
