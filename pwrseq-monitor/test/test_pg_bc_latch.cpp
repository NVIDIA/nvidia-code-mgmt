/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Extensive unit tests for BC level-latch under PG validity
 * (hal/pg_bc_latch.hpp — NVBUG 6559782).
 *
 * Coverage groups:
 *   EmitPowerGoodEdge_*  — pure helper event-order / side-effect contract
 *   Ooo_*                — out-of-order BC-before-PG (the filed bug)
 *   Invalidate_*         — latch-off on PG drop / no stale Online
 *   LatchOffBc_*         — latch-off on BC drop (BootRegress path)
 *   PowerCycle_*         — chassis / PgoodDrop round-trips (incl. off→on
 *                          OoO recover without harness/daemon restart)
 *   Seed_*               — daemon-start both-lines-high recovery
 *   MultiElement_*       — instance isolation
 *   Timer_*              — T_BOOT arm/cancel interaction with synth BcHi
 *   StickyBcRecover_*    — T_BOOT/reset timeout gpio-mon level recovery
 */

#include "fsm/element_fsm.hpp"
#include "hal/mock_device.hpp"
#include "hal/pg_bc_latch.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace pwrseq;
using namespace pwrseq::fsm;
using namespace pwrseq::hal;
using namespace pwrseq::hal::test;

namespace
{

constexpr InstanceId kLpu0{InstanceId::Class::Lpu, 0};
constexpr InstanceId kLpu7{InstanceId::Class::Lpu, 7};
constexpr InstanceId kFpga0{InstanceId::Class::Fpga, 0};

/** Sink that records events and optionally forwards into an ElementFsm. */
class FsmSink : public SemanticEventSink
{
  public:
    explicit FsmSink(ElementFsm* fsm = nullptr) : fsm_(fsm) {}

    void emit(const SemanticEvent& ev) override
    {
        events.push_back(ev);
        if (fsm_ != nullptr)
        {
            fsm_->dispatch(ev);
        }
    }

    std::vector<EventKind> kinds() const
    {
        std::vector<EventKind> out;
        out.reserve(events.size());
        for (const auto& e : events)
        {
            out.push_back(e.kind);
        }
        return out;
    }

    std::vector<SemanticEvent> events;

  private:
    ElementFsm* fsm_;
};

/** Per-element harness mirroring LpuDriver/FpgaDriver PG+BC latch wiring.
 *  Non-copyable / non-movable: FsmSink holds a raw pointer to fsm.
 */
struct Harness
{
    InstanceId id;
    ElementFsm fsm;
    FsmSink    sink;
    bool       bcLatched = false;
    int        startTBootCalls = 0;
    int        cancelTBootCalls = 0;
    int        invalidateCalls = 0;

    explicit Harness(InstanceId instance = kLpu0) :
        id(instance), fsm(instance), sink(&fsm)
    {
        fsm.setStartTBoot([this]() { ++startTBootCalls; });
        fsm.setCancelTBoot([this]() { ++cancelTBootCalls; });
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    void pgEdge(bool asserted)
    {
        emitPowerGoodEdge(sink, id, asserted, bcLatched, [this] {
            ++invalidateCalls;
            bcLatched = false;
        });
    }

    void bcEdge(bool asserted)
    {
        bcLatched = asserted;
        sink.emit(SemanticEvent{asserted ? EventKind::BcHi : EventKind::BcLo,
                                id});
    }

    /** Simulate reset-assert BC cache clear (Orchestrator path). */
    void clearBcOnReset()
    {
        ++invalidateCalls;
        bcLatched = false;
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Pure emitPowerGoodEdge contract
// ═══════════════════════════════════════════════════════════════════════════

TEST(EmitPowerGoodEdge, RisingWithoutBc_EmitsOnlyPgHi)
{
    FsmSink sink;
    int     inv = 0;
    emitPowerGoodEdge(sink, kLpu0, true, false, [&] { ++inv; });
    EXPECT_EQ(inv, 0);
    EXPECT_EQ(sink.kinds(), (std::vector<EventKind>{EventKind::PgHi}));
    EXPECT_EQ(sink.events[0].target.index, kLpu0.index);
    EXPECT_EQ(sink.events[0].target.cls, InstanceId::Class::Lpu);
}

TEST(EmitPowerGoodEdge, RisingWithBc_EmitsPgHiThenBcHi)
{
    FsmSink sink;
    int     inv = 0;
    emitPowerGoodEdge(sink, kLpu0, true, true, [&] { ++inv; });
    EXPECT_EQ(inv, 0);
    EXPECT_EQ(sink.kinds(),
              (std::vector<EventKind>{EventKind::PgHi, EventKind::BcHi}));
    EXPECT_EQ(sink.events[0].target.index, kLpu0.index);
    EXPECT_EQ(sink.events[1].target.index, kLpu0.index);
}

TEST(EmitPowerGoodEdge, Falling_InvalidatesThenEmitsPgLo)
{
    FsmSink sink;
    bool    latched = true;
    bool    clearedBeforeEmit = false;

    struct OrderSink : SemanticEventSink
    {
        FsmSink& inner;
        bool&    latched;
        bool&    flag;
        OrderSink(FsmSink& i, bool& l, bool& f) : inner(i), latched(l), flag(f)
        {}
        void emit(const SemanticEvent& ev) override
        {
            if (ev.kind == EventKind::PgLo && !latched)
            {
                flag = true;
            }
            inner.emit(ev);
        }
    } order(sink, latched, clearedBeforeEmit);

    emitPowerGoodEdge(order, kFpga0, false, /*bcAlreadyAsserted=*/true,
                      [&] { latched = false; });

    EXPECT_TRUE(clearedBeforeEmit);
    EXPECT_FALSE(latched);
    EXPECT_EQ(sink.kinds(), (std::vector<EventKind>{EventKind::PgLo}));
    EXPECT_EQ(sink.events[0].target.cls, InstanceId::Class::Fpga);
    EXPECT_EQ(sink.events[0].target.index, kFpga0.index);
}

TEST(EmitPowerGoodEdge, Falling_IgnoresBcAlreadyAssertedFlag)
{
    // bcAlreadyAsserted must not synthesize anything on the falling path.
    FsmSink sink;
    int     inv = 0;
    emitPowerGoodEdge(sink, kLpu0, false, /*bcAlreadyAsserted=*/true,
                      [&] { ++inv; });
    EXPECT_EQ(inv, 1);
    EXPECT_EQ(sink.kinds(), (std::vector<EventKind>{EventKind::PgLo}));
}

TEST(EmitPowerGoodEdge, Rising_DoesNotCallInvalidate)
{
    FsmSink sink;
    int     inv = 0;
    emitPowerGoodEdge(sink, kLpu0, true, true, [&] { ++inv; });
    emitPowerGoodEdge(sink, kLpu0, true, false, [&] { ++inv; });
    EXPECT_EQ(inv, 0);
}

TEST(EmitPowerGoodEdge, Falling_CallsInvalidateExactlyOnce)
{
    FsmSink sink;
    int     inv = 0;
    emitPowerGoodEdge(sink, kLpu0, false, false, [&] { ++inv; });
    EXPECT_EQ(inv, 1);
}

TEST(EmitPowerGoodEdge, PreservesFpgaInstanceIdOnSynthBcHi)
{
    FsmSink sink;
    emitPowerGoodEdge(sink, kFpga0, true, true, [] {});
    ASSERT_EQ(sink.events.size(), 2u);
    EXPECT_EQ(sink.events[1].kind, EventKind::BcHi);
    EXPECT_EQ(sink.events[1].target.cls, InstanceId::Class::Fpga);
    EXPECT_EQ(sink.events[1].target.index, 0u);
}

TEST(EmitPowerGoodEdge, PreservesHighIndexLpuId)
{
    FsmSink sink;
    emitPowerGoodEdge(sink, kLpu7, true, true, [] {});
    ASSERT_EQ(sink.events.size(), 2u);
    EXPECT_EQ(sink.events[0].target.index, 7u);
    EXPECT_EQ(sink.events[1].target.index, 7u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Out-of-order arrivals (NVBUG 6559782)
// ═══════════════════════════════════════════════════════════════════════════

TEST(Ooo, BcBeforePg_ReachesOnline)
{
    Harness h;
    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);

    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.cancelTBootCalls, 1);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.invalidateCalls, 0);
}

TEST(Ooo, BcBeforePg_EventOrderIsBcThenPgThenSynthBc)
{
    Harness h;
    h.bcEdge(true);
    h.sink.events.clear();
    h.pgEdge(true);
    EXPECT_EQ(h.sink.kinds(),
              (std::vector<EventKind>{EventKind::PgHi, EventKind::BcHi}));
}

TEST(Ooo, BcHiBcLoBeforePg_StaysPowered)
{
    // BC pulsed while still Standby; latch ends low → no synth on PgHi.
    Harness h;
    h.bcEdge(true);
    h.bcEdge(false);
    EXPECT_FALSE(h.bcLatched);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
}

TEST(Ooo, BcLoBcHiBeforePg_ReachesOnline)
{
    Harness h;
    h.bcEdge(true);
    h.bcEdge(false);
    h.bcEdge(true);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Ooo, InOrderPgThenBc_StillReachesOnline)
{
    Harness h;
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Ooo, SimultaneousLevels_PgRiseWithBcAlreadyHigh)
{
    // Seed / both lines already high: no prior BcHi edge into the FSM,
    // only the latch flag is set (SharedLine hasValue && read).
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Ooo, ThirteenOfSixteenPattern_AffectedRecoverUnaffectedStayOnline)
{
    // Reproduce the bug report shape: many LPUs OOO-stuck without the
    // latch; with the latch they all reach Online.
    std::vector<std::unique_ptr<Harness>> lpus;
    lpus.reserve(16);
    for (uint16_t i = 0; i < 16; ++i)
    {
        lpus.push_back(
            std::make_unique<Harness>(InstanceId{InstanceId::Class::Lpu, i}));
    }

    // LPU 0..12: OOO (BC before PG). LPU 13..15: in-order.
    for (size_t i = 0; i < 13; ++i)
    {
        lpus[i]->bcEdge(true);
        lpus[i]->pgEdge(true);
    }
    for (size_t i = 13; i < 16; ++i)
    {
        lpus[i]->pgEdge(true);
        lpus[i]->bcEdge(true);
    }

    for (size_t i = 0; i < 16; ++i)
    {
        EXPECT_EQ(lpus[i]->fsm.state(), ElementState::Online) << "lpu/" << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Latch invalidate on PG drop
// ═══════════════════════════════════════════════════════════════════════════

TEST(Invalidate, PgLo_ClearsLatchBeforeFault)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Online);

    h.bcLatched = true; // electrical BC still high / stale
    h.pgEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.invalidateCalls, 1);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);
}

TEST(Invalidate, AfterPgoodDrop_NextPgHiDoesNotShortcutOnStaleLatch)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    h.bcLatched = true;
    h.pgEdge(false);
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);

    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    EXPECT_FALSE(h.bcLatched);
}

TEST(Invalidate, AfterPgoodDrop_FreshBcHiAfterPgHi_ReachesOnline)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    h.bcLatched = true;
    h.pgEdge(false);
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Powered);

    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Invalidate, AfterPgoodDrop_OooBcBeforeSecondPgHi_ReachesOnline)
{
    // Second power-up can also be OOO: BC re-asserts before PG returns.
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    h.pgEdge(false);
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);

    // BC arrives while still Fault(PgoodDrop): FSM ignores bare BcHi,
    // but latch is set; subsequent PgHi must synthesize Online.
    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Invalidate, PoweredPgLo_ClearsLatchAndReturnsStby)
{
    Harness h;
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Powered);
    h.bcLatched = true; // would-be stale if not cleared
    h.pgEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_EQ(h.invalidateCalls, 1);
}

TEST(Invalidate, ResetAssertCacheClear_PreventsStaleSynthOnNextPgHi)
{
    // Mirrors Orchestrator::clearBootCompleteCache before reset wait.
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Online);

    h.fsm.dispatch(SemanticEvent{EventKind::RstAsserted, h.id});
    h.clearBcOnReset();
    EXPECT_FALSE(h.bcLatched);
    h.fsm.dispatch(SemanticEvent{EventKind::RstReleased, h.id});
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);

    // Without a fresh BC edge / latch, stay Powered.
    // (If cache were not cleared, a re-sample would wrongly go Online.)
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);

    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Invalidate, DoublePgLo_InvalidatesEachTime)
{
    Harness h;
    h.pgEdge(true);
    h.pgEdge(false);
    h.pgEdge(false); // Standby PgLo self-transition
    EXPECT_EQ(h.invalidateCalls, 2);
    EXPECT_FALSE(h.bcLatched);
}

// ═══════════════════════════════════════════════════════════════════════════
// Latch-off on BC signal drop
// ═══════════════════════════════════════════════════════════════════════════

TEST(LatchOffBc, BcLoWhileOnline_FaultsBootRegressAndClearsLatch)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Online);

    h.bcEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootRegress);
}

TEST(LatchOffBc, BootRegress_BareBcHiRecoversOnline)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    h.bcEdge(false);
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::BootRegress);

    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_TRUE(h.bcLatched);
}

TEST(LatchOffBc, BcLoWhilePowered_KeepsPoweredClearsLatch)
{
    Harness h;
    h.pgEdge(true);
    h.bcLatched = true;
    h.bcEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
}

TEST(LatchOffBc, BcLoWhileStby_ClearsLatch)
{
    Harness h;
    h.bcEdge(true);
    h.bcEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
}

// ═══════════════════════════════════════════════════════════════════════════
// Power-cycle / BootTimeout interaction
// ═══════════════════════════════════════════════════════════════════════════

TEST(PowerCycle, HungSecondBoot_StillFaultsViaTBoot)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    h.pgEdge(false);
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Powered);

    h.fsm.dispatch(SemanticEvent{EventKind::TBootExpired, h.id});
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);
    EXPECT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);
}

TEST(PowerCycle, BootTimeoutThenLateBc_RecoversOnline)
{
    Harness h;
    h.pgEdge(true);
    h.fsm.dispatch(SemanticEvent{EventKind::TBootExpired, h.id});
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::BootTimeout);

    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(PowerCycle, BootTimeoutThenPgLoInvalidate_NextOooRecovers)
{
    Harness h;
    h.pgEdge(true);
    h.fsm.dispatch(SemanticEvent{EventKind::TBootExpired, h.id});
    h.bcLatched = true; // stale
    h.pgEdge(false);
    EXPECT_FALSE(h.bcLatched);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);

    h.bcEdge(true);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(PowerCycle, ThreeConsecutiveOooCycles_AllReachOnline)
{
    Harness h;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        h.bcEdge(true);
        h.pgEdge(true);
        ASSERT_EQ(h.fsm.state(), ElementState::Online) << "cycle " << cycle;
        h.pgEdge(false);
        ASSERT_EQ(h.fsm.state(), ElementState::Fault) << "cycle " << cycle;
        ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop) << "cycle " << cycle;
    }
}

TEST(PowerCycle, FromPowered_PgLoThenOoo_ReachesOnline)
{
    Harness h;
    h.pgEdge(true);
    h.pgEdge(false);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    h.bcEdge(true);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(PowerCycle, ChassisOffOn_OooBcAlreadyHigh_RecoversWithoutRestart)
{
    // Bench Case A (NVBUG 6559782): same daemon instance (Harness), chassis
    // power off→on. BC may assert before PG on the way back up; latch must
    // synthesize BcHi under the fresh PgHi so we reach Online (poll_mask
    // would become non-zero on hardware). No harness rebuild = no monitord
    // restart.
    Harness h;

    // First boot (in-order or OoO — either is fine).
    h.bcEdge(true);
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Online);

    // Chassis power off: PG drops → PgoodDrop fault; invalidate clears latch.
    h.pgEdge(false);
    ASSERT_EQ(h.fsm.state(), ElementState::Fault);
    ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop);
    EXPECT_FALSE(h.bcLatched);

    // Power still off / early on: BOOT_COMPLETE can rise before MODULE_PGOOD.
    h.bcEdge(true);
    EXPECT_TRUE(h.bcLatched);
    // Still Fault(PgoodDrop): bare BcHi must NOT shortcut to Online.
    EXPECT_EQ(h.fsm.state(), ElementState::Fault);

    // Chassis power on: PG rises; synth BcHi under PG validity → Online.
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_TRUE(h.bcLatched);
}

TEST(PowerCycle, ChassisOffOn_ThreeCycles_AllRecoverOnline)
{
    Harness h;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        h.bcEdge(true);
        h.pgEdge(true);
        ASSERT_EQ(h.fsm.state(), ElementState::Online) << "cycle " << cycle;

        h.pgEdge(false);
        ASSERT_EQ(h.fsm.faultCode(), FaultCode::PgoodDrop) << "cycle " << cycle;

        h.bcEdge(true); // OoO BC-before-PG on next power-on
        h.pgEdge(true);
        ASSERT_EQ(h.fsm.state(), ElementState::Online) << "recover cycle " << cycle;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Seed / daemon-restart recovery
// ═══════════════════════════════════════════════════════════════════════════

TEST(Seed, BothHigh_PgFirstThenBc_Online)
{
    Harness h;
    h.pgEdge(true);
    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Seed, BothHigh_BcLatchedThenPg_Online)
{
    // gpio-mon seed can inject BC before PG depending on line order.
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Seed, PgHighBcLow_StaysPoweredUntilBc)
{
    Harness h;
    h.bcLatched = false;
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
    h.bcEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Seed, NeitherHigh_StaysStby)
{
    Harness h;
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
    EXPECT_EQ(h.sink.events.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-element isolation
// ═══════════════════════════════════════════════════════════════════════════

TEST(MultiElement, LatchOnOneDoesNotAffectSibling)
{
    Harness a{{InstanceId::Class::Lpu, 0}};
    Harness b{{InstanceId::Class::Lpu, 1}};

    a.bcEdge(true);
    a.pgEdge(true);
    EXPECT_EQ(a.fsm.state(), ElementState::Online);

    b.pgEdge(true);
    EXPECT_EQ(b.fsm.state(), ElementState::Powered);
    EXPECT_EQ(a.fsm.state(), ElementState::Online);
}

TEST(MultiElement, InvalidateOnOneDoesNotClearSiblingLatch)
{
    Harness a{{InstanceId::Class::Lpu, 0}};
    Harness b{{InstanceId::Class::Lpu, 1}};

    a.bcLatched = true;
    a.pgEdge(true);
    b.bcLatched = true;
    b.pgEdge(true);
    ASSERT_EQ(a.fsm.state(), ElementState::Online);
    ASSERT_EQ(b.fsm.state(), ElementState::Online);

    a.pgEdge(false);
    EXPECT_FALSE(a.bcLatched);
    EXPECT_TRUE(b.bcLatched);
    EXPECT_EQ(b.fsm.state(), ElementState::Online);
}

TEST(MultiElement, FpgaOoo_ReachesOnline)
{
    Harness h{kFpga0};
    h.bcEdge(true);
    h.pgEdge(true);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
    EXPECT_EQ(h.sink.events.back().target.cls, InstanceId::Class::Fpga);
}

// ═══════════════════════════════════════════════════════════════════════════
// Timer interaction
// ═══════════════════════════════════════════════════════════════════════════

TEST(Timer, OooPath_ArmsThenCancelsTBoot)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.cancelTBootCalls, 1);
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}

TEST(Timer, PgOnly_ArmsTBootWithoutCancel)
{
    Harness h;
    h.pgEdge(true);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.cancelTBootCalls, 0);
}

TEST(Timer, InOrder_ArmsThenCancelsOnBc)
{
    Harness h;
    h.pgEdge(true);
    h.bcEdge(true);
    EXPECT_EQ(h.startTBootCalls, 1);
    EXPECT_EQ(h.cancelTBootCalls, 1);
}

TEST(Timer, PgoodDropRecovery_RearmsTBoot)
{
    Harness h;
    h.bcLatched = true;
    h.pgEdge(true);
    const int startsAfterOnline = h.startTBootCalls;
    h.pgEdge(false);
    h.pgEdge(true);
    EXPECT_EQ(h.startTBootCalls, startsAfterOnline + 1);
    EXPECT_EQ(h.fsm.state(), ElementState::Powered);
}

TEST(Timer, PgLoFromPowered_CancelsTBoot)
{
    Harness h;
    h.pgEdge(true);
    ASSERT_EQ(h.startTBootCalls, 1);
    h.pgEdge(false);
    EXPECT_EQ(h.cancelTBootCalls, 1);
    EXPECT_EQ(h.fsm.state(), ElementState::Stby);
}

// ═══════════════════════════════════════════════════════════════════════════
// MockDevice clearBootCompleteCache contract (reset / PgLo helper)
// ═══════════════════════════════════════════════════════════════════════════

TEST(MockDeviceCache, ClearBootCompleteCache_ForcesFalse)
{
    MockDevice dev(kLpu0);
    dev.status.bootComplete = true;
    dev.clearBootCompleteCache();
    EXPECT_FALSE(dev.get_status().bootComplete);
}

TEST(MockDeviceCache, ClearThenSample_MatchesInvalidateSemantics)
{
    // Driver rising path: sample get_status().bootComplete after prior clear.
    MockDevice dev(kLpu0);
    RecordingSink sink;
    ASSERT_EQ(dev.init(sink, {}), Result::Ok);

    dev.status.bootComplete = true;
    dev.clearBootCompleteCache();
    const bool bcHigh = dev.get_status().bootComplete;
    emitPowerGoodEdge(sink, kLpu0, true, bcHigh, [&] {
        dev.clearBootCompleteCache();
    });
    EXPECT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].kind, EventKind::PgHi);
}

// ═══════════════════════════════════════════════════════════════════════════
// Sticky BC recovery on T_BOOT / reset timeout (FPGA CONF_DONE)
// ═══════════════════════════════════════════════════════════════════════════

TEST(StickyBcRecover, ElectricalHighActiveHigh_RecoversAndInjects)
{
    std::optional<bool> injected;
    EXPECT_TRUE(tryRecoverStickyBootComplete(
        true, /*activeLow=*/false, [&](bool e) {
            injected = e;
            return true;
        }));
    ASSERT_TRUE(injected.has_value());
    EXPECT_TRUE(*injected);
}

TEST(StickyBcRecover, ElectricalLowActiveHigh_DoesNotRecover)
{
    int calls = 0;
    EXPECT_FALSE(tryRecoverStickyBootComplete(
        false, /*activeLow=*/false, [&](bool) {
            ++calls;
            return true;
        }));
    EXPECT_EQ(calls, 0);
}

TEST(StickyBcRecover, ElectricalLowActiveLow_Recovers)
{
    // active-low: electrical 0 → logical asserted
    std::optional<bool> injected;
    EXPECT_TRUE(tryRecoverStickyBootComplete(
        false, /*activeLow=*/true, [&](bool e) {
            injected = e;
            return true;
        }));
    ASSERT_TRUE(injected.has_value());
    EXPECT_FALSE(*injected);
}

TEST(StickyBcRecover, MissingGpioMonRead_DoesNotRecover)
{
    int calls = 0;
    EXPECT_FALSE(tryRecoverStickyBootComplete(
        std::nullopt, false, [&](bool) {
            ++calls;
            return true;
        }));
    EXPECT_EQ(calls, 0);
}

TEST(StickyBcRecover, InjectFailure_DoesNotClaimRecovered)
{
    EXPECT_FALSE(tryRecoverStickyBootComplete(
        true, false, [](bool) { return false; }));
}

TEST(StickyBcRecover, SynthesizedBcHi_FromPowered_GoesOnline)
{
    // Mirrors orchestrator: on T_BOOT expiry, inject BcHi instead of
    // TBootExpired when gpio-mon still shows BC high.
    Harness h;
    h.pgEdge(true);
    ASSERT_EQ(h.fsm.state(), ElementState::Powered);

    EXPECT_TRUE(tryRecoverStickyBootComplete(
        true, false, [&](bool) {
            h.bcEdge(true);
            return true;
        }));
    EXPECT_EQ(h.fsm.state(), ElementState::Online);
}
