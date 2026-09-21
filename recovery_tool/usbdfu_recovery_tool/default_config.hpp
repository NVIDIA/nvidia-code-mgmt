// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include "config.h"

#include "perform_dfu_recovery.hpp"

#include <cstdint>

namespace usbdfu
{

/**
 * Build a Config from the platform meson options (config.h).  GPIO names are
 * not part of it: the CLI requires them on the command line and the D-Bus
 * worker reads them from Entity Manager.
 */
inline UsbDfuRecovery::Config makeDefaultConfig()
{
    UsbDfuRecovery::Config cfg;
    cfg.dfuUtilPath = USBDFU_UTIL_PATH;
    cfg.dfuAltSetting = USBDFU_RECOVERY_DFU_ALT;
    cfg.flashLengthBytes =
        static_cast<std::uintmax_t>(USBDFU_RECOVERY_FLASH_LENGTH);
    cfg.postFlashSettleSecs = USBDFU_RECOVERY_POST_FLASH_SETTLE;
    return cfg;
}

} // namespace usbdfu
