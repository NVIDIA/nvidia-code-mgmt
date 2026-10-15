/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Module FSM unit tests. Covers every row of the §2.2.2 matrix plus the
 * aggregation truth table in design §6.1 (per-element + module rollup).
 */

#include "fsm/module_fsm.hpp"

#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace pwrseq;
using namespace pwrseq::fsm;

namespace
{

constexpr InstanceId kModule0{InstanceId::Class::Module, 0};
constexpr InstanceId kLpuA{InstanceId::Class::Lpu, 1};
constexpr InstanceId kLpuB{InstanceId::Class::Lpu, 2};

struct Harness
{
    ModuleFsm fsm{kModule0, kLpuA, kLpuB};
    std::vector<std::pair<ModuleState, ModuleState>> transitions;

    Harness()
    {
        fsm.setOnStateChange([this](ModuleState a, ModuleState b) {
            transitions.emplace_back(a, b);
        });
    }

    void childA(ElementState s)
    {
        fsm.dispatch(SemanticEvent{
            EventKind::LpuStateChanged,
            kModule0,
            ChildElementPayload{kLpuA, s},
        });
    }
    void childB(ElementState s)
    {
        fsm.dispatch(SemanticEvent{
            EventKind::LpuStateChanged,
            kModule0,
            ChildElementPayload{kLpuB, s},
        });
    }
};

} // namespace

// ─── per-row tests ─────────────────────────────────────────────────────────

TEST(ModuleFsm, InitialStateIsStby)
{
    Harness h;
    EXPECT_EQ(h.fsm.state(), ModuleState::Stby);
}

TEST(ModuleFsm, Stby_AnyChildPowered_GoesPowered)
{
    Harness h;
    h.childA(ElementState::Powered);
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
}

TEST(ModuleFsm, Powered_BothOnline_GoesOnline)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
    h.childA(ElementState::Online);
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered); // only one online
    h.childB(ElementState::Online);
    EXPECT_EQ(h.fsm.state(), ModuleState::Online);
}

TEST(ModuleFsm, Powered_AnyFault_GoesFault)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Fault);
    EXPECT_EQ(h.fsm.state(), ModuleState::Fault);
}

TEST(ModuleFsm, Online_AnyChildLeavesOnline_GoesFault)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    ASSERT_EQ(h.fsm.state(), ModuleState::Online);
    h.childB(ElementState::Fault);
    EXPECT_EQ(h.fsm.state(), ModuleState::Fault);
}

TEST(ModuleFsm, Online_BothResetting_GoesResetting)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    h.childA(ElementState::Resetting);
    h.childB(ElementState::Resetting);
    EXPECT_EQ(h.fsm.state(), ModuleState::Resetting);
}

TEST(ModuleFsm, Resetting_AnyChildPoweredAgain_GoesPowered)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    h.childA(ElementState::Resetting);
    h.childB(ElementState::Resetting);
    h.childA(ElementState::Powered); // came back from reset
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
}

TEST(ModuleFsm, Resetting_AnyChildFault_GoesFault)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    h.childA(ElementState::Resetting);
    h.childB(ElementState::Resetting);
    h.childB(ElementState::Fault);
    EXPECT_EQ(h.fsm.state(), ModuleState::Fault);
}

TEST(ModuleFsm, Resetting_BothStby_GoesStby)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    h.childA(ElementState::Resetting);
    h.childB(ElementState::Resetting);
    h.childA(ElementState::Stby);
    h.childB(ElementState::Stby);
    EXPECT_EQ(h.fsm.state(), ModuleState::Stby);
}

TEST(ModuleFsm, Fault_AllChildrenOnline_LateRecovery_GoesOnline)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Fault);
    ASSERT_EQ(h.fsm.state(), ModuleState::Fault);
    h.childA(ElementState::Online); // late recovery
    h.childB(ElementState::Online);
    EXPECT_EQ(h.fsm.state(), ModuleState::Online);
}

TEST(ModuleFsm, Fault_OperatorReset_ChildPowered_GoesPowered)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Fault);
    h.childA(ElementState::Resetting); // operator started a reset
    h.childA(ElementState::Powered);   // reset released
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
}

TEST(ModuleFsm, Fault_BothPowerDown_GoesStby)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Fault);
    h.childA(ElementState::Stby);
    h.childB(ElementState::Stby);
    EXPECT_EQ(h.fsm.state(), ModuleState::Stby);
}

// ─── per the §6.1 module aggregation truth table ──────────────────────────

TEST(ModuleFsm, Aggregation_OnePoweredOneOnline_StaysPowered)
{
    // Per §6.1: LPU_A Online + LPU_B Powered → Module S_POWERED (still
    // progressing), not S_ONLINE.
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
}

TEST(ModuleFsm, Aggregation_OneOnlineOneResetting_StaysOnline)
{
    // Per §6.1: one mid-reset doesn't fault the module. It stays M_ONLINE
    // until the other also goes to S_RESETTING (which would be a mass reset).
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    h.childB(ElementState::Resetting);
    EXPECT_EQ(h.fsm.state(), ModuleState::Online);
}

TEST(ModuleFsm, Aggregation_OnlineThenChildPowered_DropsToPowered)
{
    // Per-element warm reset on a 2-child module: one child completes its
    // reset pulse and returns to S_POWERED (waiting for BC) while the
    // other stays S_ONLINE. The module aggregate is no longer fully
    // online and should drop to M_POWERED so any boot-complete observer
    // sees the transition.
    //
    // Regression for the "Online + Powered" combo that previously left
    // the module wedged in M_ONLINE with stale child state
    // {Online, Powered}. (CodeRabbit MR !1167 review #25.)
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Online);
    h.childB(ElementState::Online);
    ASSERT_EQ(h.fsm.state(), ModuleState::Online);
    // Per-LPU reset on B: B goes Online -> Resetting -> Powered. The
    // intermediate Resetting transition stays in Online (covered by the
    // OneOnlineOneResetting test above); the final {Online, Powered}
    // combo should drop the module to Powered.
    h.childB(ElementState::Resetting);
    EXPECT_EQ(h.fsm.state(), ModuleState::Online);
    h.childB(ElementState::Powered);
    EXPECT_EQ(h.fsm.state(), ModuleState::Powered);
}

TEST(ModuleFsm, Aggregation_BothFault_StaysFault)
{
    Harness h;
    h.childA(ElementState::Powered);
    h.childB(ElementState::Powered);
    h.childA(ElementState::Fault);
    h.childB(ElementState::Fault);
    EXPECT_EQ(h.fsm.state(), ModuleState::Fault);
}

// ─── unknown / mis-routed events ──────────────────────────────────────────

TEST(ModuleFsm, IgnoresEventsForUnknownChild)
{
    Harness h;
    constexpr InstanceId notOurs{InstanceId::Class::Lpu, 99};
    h.fsm.dispatch(SemanticEvent{
        EventKind::LpuStateChanged,
        kModule0,
        ChildElementPayload{notOurs, ElementState::Powered},
    });
    EXPECT_EQ(h.fsm.state(), ModuleState::Stby);
}

TEST(ModuleFsm, IgnoresIrrelevantEventKinds)
{
    Harness h;
    h.fsm.dispatch(SemanticEvent{EventKind::PgHi, kModule0}); // wrong kind
    EXPECT_EQ(h.fsm.state(), ModuleState::Stby);
}

// ─── table sanity ──────────────────────────────────────────────────────────

TEST(ModuleFsmTable, NonEmpty)
{
    auto rows = moduleFsmTable();
    EXPECT_GT(rows.size(), 0u);
}
