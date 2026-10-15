/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <string_view>
#include <variant>

namespace pwrseq
{

/** Identifies which FSM instance an event is targeted at.
 *
 *  `index` is ZERO-BASED in code, while the hardware silkscreen and Redfish
 *  identifiers (descriptor.id strings like "HGX_LPU_3") are typically
 *  presented as 1-based labels. Mapping at the boundary:
 *
 *      InstanceId{Class::Lpu, 0}  ↔ silkscreen "LPU 1"  ↔ descriptor.id "HGX_LPU_0"
 *      InstanceId{Class::Lpu, 1}  ↔ silkscreen "LPU 2"  ↔ descriptor.id "HGX_LPU_1"
 *      ...
 *      InstanceId{Class::Fpga, 0} ↔ silkscreen "FPGA 0" ↔ descriptor.id "HGX_FPGA_0"
 *
 *  All in-process maps (deviceByInstance_, elementByInstance_, ...) key on
 *  this zero-based index. Translation happens at:
 *    - descriptor_loader (parses descriptor.id, assigns zero-based InstanceId)
 *    - Orchestrator::descriptorIdByInstance / instanceByDescriptorId
 *    - LpuDriver / FpgaDriver lg2 logs (which print both forms)
 */
struct InstanceId
{
    enum class Class : uint8_t
    {
        Lpu,
        Fpga,
        Module,
        Tray,
    };

    Class    cls{};
    uint16_t index{};  // 0..15 for LPU; 0..1 for FPGA; 0..7 for Module; 0 for Tray

    auto operator<=>(const InstanceId&) const = default;
};

std::string_view to_string(InstanceId::Class c) noexcept;
std::string      to_string(InstanceId id);

/** Element FSM states (LPU and FPGA both run the Element FSM).
 *  See design doc §2.1.
 */
enum class ElementState : uint8_t
{
    Stby,       // S_STBY      — no power; nothing to monitor
    Powered,    // S_POWERED   — PG up; awaiting BC; T_BOOT running
    Online,     // S_ONLINE    — BC asserted; operational
    Resetting,  // S_RESETTING — vHMC drove reset low (transient)
    Fault,      // S_FAULT     — T_BOOT expired, or BC/PG dropped while online
};
std::string_view to_string(ElementState s) noexcept;

/** Module FSM states. See design doc §2.2. */
enum class ModuleState : uint8_t
{
    Stby,
    Powered,
    Online,
    Resetting,
    Fault,
};
std::string_view to_string(ModuleState s) noexcept;

/** Tray FSM states. See design doc §2.3. */
enum class TrayState : uint8_t
{
    Stby,
    Booting,
    Online,
    Fault,
    Maintenance,
};
std::string_view to_string(TrayState s) noexcept;

/** Event kinds the FSM core understands.
 *  Hardware-edge events (PG/BC/RST) are emitted by the HAL drivers (§8).
 *  Aggregator inputs (LpuStateChanged, ModuleStateChanged, FpgaStateChanged)
 *  are emitted by child FSMs.
 */
enum class EventKind : uint8_t
{
    // ---- Element FSM inputs ----
    PgHi,           // EV_PG_HI         — power-good asserted
    PgLo,           // EV_PG_LO         — power-good deasserted
    BcHi,           // EV_BC_HI         — boot-complete asserted
    BcLo,           // EV_BC_LO         — boot-complete deasserted
    RstAsserted,    // EV_RST_ASSERTED  — engine drove reset low
    RstReleased,    // EV_RST_RELEASED  — engine released reset
    TBootExpired,   // EV_T_BOOT_EXPIRED — boot deadline timer fired

    // ---- Module FSM input ----
    LpuStateChanged,     // EV_LPU_STATE_CHANGED — payload: ChildElementPayload

    // ---- Tray FSM inputs ----
    ModuleStateChanged,  // EV_MODULE_STATE_CHANGED — payload: ChildModulePayload
    FpgaStateChanged,    // EV_FPGA_STATE_CHANGED   — payload: ChildElementPayload
    OpMaintenanceEnter,  // EV_OPERATOR_MAINTENANCE_ENTER — Redfish OEM action
    OpMaintenanceExit,   // EV_OPERATOR_MAINTENANCE_EXIT
};
std::string_view to_string(EventKind e) noexcept;

/** Fault codes attached to S_FAULT, produced by the FSM itself.
 *  The BOOT_HALTED_* values are retained for wire/log compatibility but are
 *  never produced: boot-status bits are owned by Redfish reading gpio-mon
 *  directly, so this daemon does not observe them.
 */
enum class FaultCode : uint8_t
{
    None = 0,
    BootTimeout,                    // BOOT_TIMEOUT
    BootHaltedDtlfsrMismatch,       // 4-bit code 5 (not produced)
    BootHaltedMemRepairTimeout,     // 4-bit code 6 (not produced)
    BootHaltedPromLoadFailure,      // 4-bit code 7 (not produced)
    BootHaltedPllFailure,           // 4-bit code 8 (not produced)
    BootHaltedLabDebugPortBoot,     // 4-bit code 9 (not produced)
    BootRegress,                    // S_ONLINE → S_FAULT via EV_BC_LO
    PgoodDrop,                      // S_ONLINE → S_FAULT via EV_PG_LO
    InitDoneTimeout,                // FPGA-side BOOT_TIMEOUT equivalent
};
std::string_view to_string(FaultCode f) noexcept;

/** Payload carried by aggregator events. */
struct ChildElementPayload
{
    InstanceId   child;
    ElementState newState;
};

struct ChildModulePayload
{
    InstanceId  child;
    ModuleState newState;
};

using EventPayload = std::variant<
    std::monostate,           // no payload (most HW edge events)
    ChildElementPayload,      // LpuStateChanged / FpgaStateChanged
    ChildModulePayload        // ModuleStateChanged
>;

/** A semantic event delivered into the FSM core.
 *  HAL drivers emit these; FSMs consume them. No raw GPIO line names,
 *  no D-Bus paths, no transport detail.
 */
struct SemanticEvent
{
    EventKind    kind;
    InstanceId   target;            // which FSM instance receives this
    EventPayload payload{};         // optional extra data (default: monostate)

    SemanticEvent(EventKind k, InstanceId t) noexcept : kind(k), target(t) {}

    SemanticEvent(EventKind k, InstanceId t, EventPayload p) noexcept :
        kind(k), target(t), payload(std::move(p))
    {}
};

}  // namespace pwrseq
