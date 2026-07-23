/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "force_recovery.hpp"
#include "recovery_mode_manager_base.hpp"

namespace nvidia::recovery
{

class USBRCMRecoveryManager : public RecoveryModeManagerBase
{
  public:
    USBRCMRecoveryManager(sdbusplus::bus_t& bus, std::string chassisName,
                          std::string objPath, RecoveryPinConfig pinConfig);

  protected:
    void performForceRecovery() override;

  private:
    bool areDevicesInRecovery() const;

    RecoveryPinConfig pinConfig;
};

} // namespace nvidia::recovery
