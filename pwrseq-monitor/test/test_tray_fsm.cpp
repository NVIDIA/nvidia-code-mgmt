/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tray FSM unit tests — design §2.3.2 transition matrix + cold-boot
 * cascade scenarios.
 */

#include "fsm/tray_fsm.hpp"

#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace pwrseq;
using namespace pwrseq::fsm;

namespace
{

constexpr InstanceId kTray{InstanceId::Class::Tray, 0};

// Use a small tray (2 modules, 1 FPGA) for tests so cold-boot scenarios are
// tractable. The FSM logic is identical regardless of count.
constexpr size_t kNumModules = 2;
constexpr size_t kNumFpgas = 1;

struct Harness
{
    TrayFsm fsm{kTray, kNumModules, kNumFpgas};
    std::vector<std::pair<TrayState, TrayState>> transitions;

    Harness()
    {
        fsm.setOnStateChange([this](TrayState a, TrayState b) {
            transitions.emplace_back(a, b);
        });
    }

    void mod(uint16_t idx, ModuleState s)
    {
        fsm.dispatch(SemanticEvent{
            EventKind::ModuleStateChanged,
            kTray,
            ChildModulePayload{InstanceId{InstanceId::Class::Module, idx}, s},
        });
    }
    void fpga(uint16_t idx, ElementState s)
    {
        fsm.dispatch(SemanticEvent{
            EventKind::FpgaStateChanged,
            kTray,
            ChildElementPayload{InstanceId{InstanceId::Class::Fpga, idx}, s},
        });
    }
    void op(EventKind k)
    {
        fsm.dispatch(SemanticEvent{k, kTray});
    }
};

} // namespace

TEST(TrayFsm, InitialStateIsStby)
{
    Harness h;
    EXPECT_EQ(h.fsm.state(), TrayState::Stby);
}

TEST(TrayFsm, Stby_AnyModulePowered_GoesBooting)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    EXPECT_EQ(h.fsm.state(), TrayState::Booting);
}

TEST(TrayFsm, Stby_AnyFpgaPowered_GoesBooting)
{
    Harness h;
    h.fpga(0, ElementState::Powered);
    EXPECT_EQ(h.fsm.state(), TrayState::Booting);
}

TEST(TrayFsm, Booting_AllChildrenOnline_GoesOnline)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.mod(1, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    ASSERT_EQ(h.fsm.state(), TrayState::Booting);

    h.mod(0, ModuleState::Online);
    h.mod(1, ModuleState::Online);
    EXPECT_EQ(h.fsm.state(), TrayState::Booting); // FPGA still booting

    h.fpga(0, ElementState::Online);
    EXPECT_EQ(h.fsm.state(), TrayState::Online);
}

TEST(TrayFsm, Booting_AnyChildFault_GoesFault)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(1, ModuleState::Fault);
    EXPECT_EQ(h.fsm.state(), TrayState::Fault);
}

TEST(TrayFsm, Online_AnyChildFault_GoesFault)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.mod(1, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(0, ModuleState::Online);
    h.mod(1, ModuleState::Online);
    h.fpga(0, ElementState::Online);
    ASSERT_EQ(h.fsm.state(), TrayState::Online);

    h.fpga(0, ElementState::Fault);
    EXPECT_EQ(h.fsm.state(), TrayState::Fault);
}

TEST(TrayFsm, Online_OpMaintEnter_GoesMaintenance)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.mod(1, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(0, ModuleState::Online);
    h.mod(1, ModuleState::Online);
    h.fpga(0, ElementState::Online);
    h.op(EventKind::OpMaintenanceEnter);
    EXPECT_EQ(h.fsm.state(), TrayState::Maintenance);
}

TEST(TrayFsm, Maintenance_OpMaintExit_GoesOnline)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.mod(1, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(0, ModuleState::Online);
    h.mod(1, ModuleState::Online);
    h.fpga(0, ElementState::Online);
    h.op(EventKind::OpMaintenanceEnter);
    h.op(EventKind::OpMaintenanceExit);
    EXPECT_EQ(h.fsm.state(), TrayState::Online);
}

TEST(TrayFsm, Fault_AllChildrenOnline_LateRecovery_GoesOnline)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(1, ModuleState::Fault); // → Fault
    ASSERT_EQ(h.fsm.state(), TrayState::Fault);

    h.mod(0, ModuleState::Online);
    h.mod(1, ModuleState::Online); // late recovery on the faulted module
    h.fpga(0, ElementState::Online);
    EXPECT_EQ(h.fsm.state(), TrayState::Online);
}

TEST(TrayFsm, Fault_AnyChildPowered_OperatorFullReset_GoesBooting)
{
    Harness h;
    h.mod(0, ModuleState::Powered);
    h.fpga(0, ElementState::Powered);
    h.mod(1, ModuleState::Fault); // → Fault
    ASSERT_EQ(h.fsm.state(), TrayState::Fault);

    // Operator triggered a full reset on module 1; it cycles through reset
    // back to Powered.
    h.mod(1, ModuleState::Resetting); // intermediate, stays Fault
    h.mod(1, ModuleState::Powered);   // back to Powered → Booting
    EXPECT_EQ(h.fsm.state(), TrayState::Booting);
}

TEST(TrayFsm, IgnoresOutOfRangeChildIndex)
{
    Harness h;
    h.mod(99, ModuleState::Powered); // beyond numModules
    EXPECT_EQ(h.fsm.state(), TrayState::Stby);
    h.fpga(7, ElementState::Powered); // beyond numFpgas
    EXPECT_EQ(h.fsm.state(), TrayState::Stby);
}

TEST(TrayFsmTable, NonEmpty)
{
    EXPECT_GT(trayFsmTable().size(), 0u);
}
