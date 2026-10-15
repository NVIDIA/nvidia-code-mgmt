/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Maps internal FSM states to D-Bus Health + OperationalStatus values per
 * design doc §6.1 (State → Status mapping table).
 */

#pragma once

#include "core/types.hpp"

#include "xyz/openbmc_project/State/Decorator/Health/server.hpp"
#include "xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp"

namespace pwrseq::reporting
{

using HealthType =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health::HealthType;
using StateType =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus::StateType;

/** What gets written to D-Bus for a given FSM state. */
struct StatusTuple
{
    HealthType health;
    StateType  state;
    bool       functional;
};

/** Map an Element FSM state to its D-Bus status tuple per §6.1 truth table.
 *  `present` distinguishes S_STBY (not powered) from "slot empty" (Absent).
 */
StatusTuple toStatus(ElementState s, bool present) noexcept;

/** Module FSM → D-Bus status. */
StatusTuple toStatus(ModuleState s) noexcept;

/** Tray FSM → D-Bus status. */
StatusTuple toStatus(TrayState s) noexcept;

}  // namespace pwrseq::reporting
