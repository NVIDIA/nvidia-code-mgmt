/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "recovery_mode_manager_base.hpp"

#include <mcu_recovery_manager.hpp>

#include <memory>
#include <string>

namespace nvidia::recovery
{

class MCURecoveryModeManager : public RecoveryModeManagerBase
{
  public:
    MCURecoveryModeManager(
        sdbusplus::bus_t& bus, std::string chassisName, std::string objPath,
        const std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>&
            mcuRecoveryMgr,
        std::string deviceId);

  protected:
    void performForceRecovery() override;

  private:
    std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>
        mcuRecoveryManager;
    std::string deviceId;
};

} // namespace nvidia::recovery
