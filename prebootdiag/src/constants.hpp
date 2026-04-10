/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace nvidia::prebootdiag::constants
{

// D-Bus constants — prebootdiag's own service
constexpr auto appService = "com.nvidia.PreBootDiag";
constexpr auto appObjPath = "/com/nvidia/prebootdiag";
constexpr auto appEnableIface = "xyz.openbmc_project.Object.Enable";
constexpr auto appNotifyIface = "com.nvidia.PreBootDiag.App";

// D-Bus constants — Settings service
constexpr auto settingsService = "xyz.openbmc_project.Settings";
constexpr auto diagObjPath = "/xyz/openbmc_project/Control/Diag";
constexpr auto diagIface = "xyz.openbmc_project.Control.Diag";
constexpr auto propsIface = "org.freedesktop.DBus.Properties";

// D-Bus constants — nsmd service. nsmd hosts the per-CPU async-config
// endpoint at /com/nvidia/prebootdiag/<eid> via com.nvidia.Async.Set
// (returning an object_path whose com.nvidia.Async.Status is watched for
// completion). The eid arrives in the payload of the
// SystemConfigRequested / TIDConfigRequested App.Notify calls, so the
// path is built per call — no static constant here.
constexpr auto nsmService = "xyz.openbmc_project.NSM";
constexpr auto nsmObjPathPrefix = "/com/nvidia/prebootdiag/";
constexpr auto nsmAsyncSetIface = "com.nvidia.Async.Set";
constexpr auto nsmAsyncStatusIface = "com.nvidia.Async.Status";
constexpr auto nsmConfigSystemIface = "com.nvidia.PreBootDiag.Config.System";
constexpr auto nsmConfigTidIface = "com.nvidia.PreBootDiag.Config.TID";
constexpr auto asyncValueProperty = "Value";

// StateType enum string values (from com.nvidia.PreBootDiag.App)
constexpr auto statePostCodeReceived =
    "com.nvidia.PreBootDiag.App.StateType.PostCodeReceived";
constexpr auto stateSystemConfigRequested =
    "com.nvidia.PreBootDiag.App.StateType.SystemConfigRequested";
constexpr auto stateTIDConfigRequested =
    "com.nvidia.PreBootDiag.App.StateType.TIDConfigRequested";
constexpr auto stateHeartbeatReceived =
    "com.nvidia.PreBootDiag.App.StateType.HeartbeatReceived";
constexpr auto stateResultReceived =
    "com.nvidia.PreBootDiag.App.StateType.ResultReceived";
constexpr auto stateSessionEnded =
    "com.nvidia.PreBootDiag.App.StateType.SessionEnded";

// PostCodeName values carried in the PostCodeReceived payload. Both must
// arrive, in this order, to satisfy the pre-NSM-loop gate.
constexpr auto postCodePscFmcBootModeDiag = "PSC_FMC_PC_BOOT_MODE_DIAG";
constexpr auto postCodeMb2CcplexPrebootDiagEntry =
    "MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY";

// D-Bus constants — Logging service
constexpr auto logService = "xyz.openbmc_project.Logging";
constexpr auto logObjPath = "/xyz/openbmc_project/logging";
constexpr auto logIface = "xyz.openbmc_project.Logging.Create";

// DiagStatus enum (wire format for the xyz.openbmc_project.Control.Diag
// DiagStatus property on Settings — numeric values must stay stable).
enum class DiagStatus : uint8_t
{
    InProgress = 0x0,
    // 0x1 reserved (legacy) — do not reuse
    Completed = 0x2,
    Abort = 0x3,
    NotStarted = 0x4,
    TestRunning = 0x5,
};

// CPU boot-chain selection GPIOs. Driven high in initGpioSequence() to
// signal both CPUs to enter preboot-diag boot; cleared at session end.
// Both lines are required — vr-vhmc-quark (single-board) does not expose
// BRD1 and will fail at enable time on this platform until a follow-up
// adds platform conditioning.
constexpr auto cpuBootChain0Brd0Gpio = "BRD0_CPU_BOOT_CHAIN0-O";
constexpr auto cpuBootChain0Brd1Gpio = "BRD1_CPU_BOOT_CHAIN0-O";

// Per-CPU system-reset GPIOs (active low). Held asserted (=0) for the
// entire window in which boot-chain straps are written, so each BootROM
// samples the strap value cleanly when reset is released (=1). See
// recovery_tool/usbrcm_recovery_tool/force_recovery.hpp for the
// three-phase rationale and the HW revisions that require it
// (BRD0 reset also drives BRD1 on some boards; BRD1's own line is NC).
constexpr auto istSysRstBrd0Gpio = "BRD0_IST_SYS_RST_L-O";
constexpr auto istSysRstBrd1Gpio = "BRD1_IST_SYS_RST_L-O";

// Timeout values (SADD Section 7.2)
// eventTimeout is the event-idle watchdog inside listenForEvents: the
// timer is reset on every NSM event (config request, heartbeat, result,
// session end); if no event arrives within this window the session is
// presumed stuck and the recovery path is taken.
constexpr auto eventTimeout = std::chrono::minutes(3);
// Per-phase post-code wait budgets.
//   PSC-FMC arrives early in the BootROM stage — short window.
//   MB2 follows after CPU memory training etc. — long window.
constexpr auto pscFmcPostCodeWaitTimeout = std::chrono::seconds(30);
constexpr auto mb2PostCodeWaitTimeout = std::chrono::minutes(15);

} // namespace nvidia::prebootdiag::constants
