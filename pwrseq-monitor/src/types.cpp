/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "core/types.hpp"

#include <sstream>

namespace pwrseq
{

std::string_view to_string(InstanceId::Class c) noexcept
{
    switch (c)
    {
        case InstanceId::Class::Lpu:
            return "lpu";
        case InstanceId::Class::Fpga:
            return "fpga";
        case InstanceId::Class::Module:
            return "module";
        case InstanceId::Class::Tray:
            return "tray";
    }
    return "?";
}

std::string to_string(InstanceId id)
{
    std::ostringstream oss;
    oss << to_string(id.cls) << '/' << id.index;
    return oss.str();
}

std::string_view to_string(ElementState s) noexcept
{
    switch (s)
    {
        case ElementState::Stby:
            return "S_STBY";
        case ElementState::Powered:
            return "S_POWERED";
        case ElementState::Online:
            return "S_ONLINE";
        case ElementState::Resetting:
            return "S_RESETTING";
        case ElementState::Fault:
            return "S_FAULT";
    }
    return "?";
}

std::string_view to_string(ModuleState s) noexcept
{
    switch (s)
    {
        case ModuleState::Stby:
            return "M_STBY";
        case ModuleState::Powered:
            return "M_POWERED";
        case ModuleState::Online:
            return "M_ONLINE";
        case ModuleState::Resetting:
            return "M_RESETTING";
        case ModuleState::Fault:
            return "M_FAULT";
    }
    return "?";
}

std::string_view to_string(TrayState s) noexcept
{
    switch (s)
    {
        case TrayState::Stby:
            return "T_STBY";
        case TrayState::Booting:
            return "T_BOOTING";
        case TrayState::Online:
            return "T_ONLINE";
        case TrayState::Fault:
            return "T_FAULT";
        case TrayState::Maintenance:
            return "T_MAINTENANCE";
    }
    return "?";
}

std::string_view to_string(EventKind e) noexcept
{
    switch (e)
    {
        case EventKind::PgHi:
            return "EV_PG_HI";
        case EventKind::PgLo:
            return "EV_PG_LO";
        case EventKind::BcHi:
            return "EV_BC_HI";
        case EventKind::BcLo:
            return "EV_BC_LO";
        case EventKind::RstAsserted:
            return "EV_RST_ASSERTED";
        case EventKind::RstReleased:
            return "EV_RST_RELEASED";
        case EventKind::TBootExpired:
            return "EV_T_BOOT_EXPIRED";
        case EventKind::LpuStateChanged:
            return "EV_LPU_STATE_CHANGED";
        case EventKind::ModuleStateChanged:
            return "EV_MODULE_STATE_CHANGED";
        case EventKind::FpgaStateChanged:
            return "EV_FPGA_STATE_CHANGED";
        case EventKind::OpMaintenanceEnter:
            return "EV_OPERATOR_MAINTENANCE_ENTER";
        case EventKind::OpMaintenanceExit:
            return "EV_OPERATOR_MAINTENANCE_EXIT";
    }
    return "?";
}

std::string_view to_string(FaultCode f) noexcept
{
    switch (f)
    {
        case FaultCode::None:
            return "NONE";
        case FaultCode::BootTimeout:
            return "BOOT_TIMEOUT";
        case FaultCode::BootHaltedDtlfsrMismatch:
            return "BOOT_HALTED_DTLFSR_MISMATCH";
        case FaultCode::BootHaltedMemRepairTimeout:
            return "BOOT_HALTED_MEM_REPAIR_TIMEOUT";
        case FaultCode::BootHaltedPromLoadFailure:
            return "BOOT_HALTED_PROM_LOAD_FAILURE";
        case FaultCode::BootHaltedPllFailure:
            return "BOOT_HALTED_PLL_FAILURE";
        case FaultCode::BootHaltedLabDebugPortBoot:
            return "BOOT_HALTED_LAB_DEBUG_PORT_BOOT";
        case FaultCode::BootRegress:
            return "BOOT_REGRESS";
        case FaultCode::PgoodDrop:
            return "PGOOD_DROP";
        case FaultCode::InitDoneTimeout:
            return "INIT_DONE_TIMEOUT";
    }
    return "?";
}

} // namespace pwrseq
