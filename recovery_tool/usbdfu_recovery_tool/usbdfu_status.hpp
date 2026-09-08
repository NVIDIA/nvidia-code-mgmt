// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include "usb_sysfs.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Recovery status of the HMC as seen from its USB recovery link.
 *
 * Like the USB RCM and MCU recovery tools, status is derived solely from the
 * device's own USB descriptors on the BMC side (sysfs) and from the DFU alt
 * settings reported by dfu-util; the tool never consults D-Bus inventory or
 * MCTP.  fw-status remains the place for platform-level health.
 *
 *   - BootROM in DFU recovery ........ USB 2245:2700 present
 *   - recovery U-Boot in DFU ......... dfu-util lists recovery_* alt settings
 *   - normally booted HMC ............ the platform's functional USB identity
 *                                      (optional, from Entity Manager)
 *   - nothing on the port ............ Unknown (dead HMC or identity not
 *                                      configured; cannot be told apart)
 */
namespace usbdfu
{

/** USB vendor:product identity, lower-case hex without "0x". */
struct UsbId
{
    std::string vid;
    std::string pid;

    /** Parse "vvvv:pppp" (case-insensitive); nullopt if malformed. */
    static std::optional<UsbId> parse(std::string_view text);
    std::string str() const
    {
        return vid + ":" + pid;
    }
};

enum class RecoveryState
{
    InRecovery,
    NotInRecovery,
    Unknown,
};

enum class RecoveryStage
{
    None,
    /** AST2700 BootROM waiting for the preliminary bundle */
    BootRom,
    /** recovery U-Boot exposing the SPI flash over DFU */
    RecoveryUBoot,
};

const char* toString(RecoveryState state) noexcept;
const char* toString(RecoveryStage stage) noexcept;

/** One "Found DFU:" line of dfu-util -l. */
struct DfuAlt
{
    std::string usbId;
    std::string path;
    std::string name;
};

struct StatusConfig
{
    std::string usbSysfsRoot;
    UsbId bootRomId;
    /** USB identity of a normally booted HMC; unset = health unobservable */
    std::optional<UsbId> functionalId;
    /** Kernel USB port path (e.g. "1-1.3") the HMC is attached to; empty =
     *  any */
    std::string usbPortFilter;
    std::string dfuUtilPath;
    int dfuListTimeoutSecs{30};
};

struct RecoveryStatus
{
    RecoveryState state{RecoveryState::Unknown};
    RecoveryStage stage{RecoveryStage::None};
    bool usbDevicePresent{false};
    std::string usbPortPath;
    std::string usbId;
    std::vector<std::string> dfuAltSettings;
    std::string error;

    /** usbrcm-style JSON ("Recovery Status", "USB Port Path", ...). */
    nlohmann::json toJson() const;
};

/** Runs argv and returns its stdout (nullopt = could not run). */
using CaptureFn = std::function<std::optional<std::string>(
    const std::vector<std::string>& argv, int timeoutSecs)>;

/** Parse dfu-util -l output into its "Found DFU:" entries. */
std::vector<DfuAlt> parseDfuUtilList(std::string_view output);

/** True for recovery U-Boot's SPI targets: recovery_cs0 / recovery_cs1 /
 * recovery_both. */
bool isRecoveryAlt(std::string_view name) noexcept;

/**
 * Pure status derivation (unit-testable):
 *   bootRom / recovery alts present   -> InRecovery (stage from the alts)
 *   functional device present         -> NotInRecovery
 *   otherwise                         -> Unknown
 */
RecoveryStatus
    deriveRecoveryStatus(const std::optional<usb_sysfs::Device>& bootRom,
                         const std::vector<DfuAlt>& dfuAlts,
                         const std::optional<usb_sysfs::Device>& functional,
                         bool functionalConfigured,
                         std::string_view portPath = {});

/** Probe sysfs and dfu-util -l and derive the status. */
RecoveryStatus getRecoveryStatus(const StatusConfig& cfg,
                                 const CaptureFn& capture);

} // namespace usbdfu
