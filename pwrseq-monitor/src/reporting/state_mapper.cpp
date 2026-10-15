/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "reporting/state_mapper.hpp"

namespace pwrseq::reporting
{

StatusTuple toStatus(ElementState s, bool /*present*/) noexcept
{
    // Binary mapping per customer scope: Redfish Status.State is either
    // "Enabled" (the LPU/FPGA element is fully up — PG asserted AND
    // Boot-Complete asserted, i.e. ElementState::Online) or "Disabled"
    // (anything else: not powered, mid-boot, mid-reset, faulted). The
    // richer Starting / StandbyOffline / UnavailableOffline distinctions
    // exist on D-Bus via the daemon's internal FSM logging, but the
    // Redfish surface intentionally collapses them to keep the
    // /Processors/LPU_<N> contract a simple two-state indicator that
    // tracks "is this LPU actually running?".
    //
    // Health is kept at OK regardless of FSM state; Redfish
    // Status.Health is owned by other producers (nvidia-monitor-eventing,
    // bmcweb file-backed OEM reader) -- this daemon only drives
    // Status.State.
    if (s == ElementState::Online)
    {
        return {HealthType::OK, StateType::Enabled, true};
    }
    return {HealthType::OK, StateType::Disabled, false};
}

StatusTuple toStatus(ModuleState s) noexcept
{
    switch (s)
    {
        case ModuleState::Stby:
            return {HealthType::OK,       StateType::StandbyOffline,     false};
        case ModuleState::Powered:
            return {HealthType::OK,       StateType::Starting,           false};
        case ModuleState::Online:
            return {HealthType::OK,       StateType::Enabled,            true};
        case ModuleState::Resetting:
            return {HealthType::OK,       StateType::StandbyOffline,     false};
        case ModuleState::Fault:
            return {HealthType::Critical, StateType::UnavailableOffline, false};
    }
    return {HealthType::Critical, StateType::UnavailableOffline, false};
}

StatusTuple toStatus(TrayState s) noexcept
{
    switch (s)
    {
        case TrayState::Stby:
            return {HealthType::OK,       StateType::StandbyOffline,     false};
        case TrayState::Booting:
            return {HealthType::OK,       StateType::Starting,           false};
        case TrayState::Online:
            return {HealthType::OK,       StateType::Enabled,            true};
        case TrayState::Fault:
            return {HealthType::Critical, StateType::UnavailableOffline, false};
        case TrayState::Maintenance:
            return {HealthType::OK,       StateType::StandbyOffline,     false};
    }
    return {HealthType::Critical, StateType::UnavailableOffline, false};
}

}  // namespace pwrseq::reporting
