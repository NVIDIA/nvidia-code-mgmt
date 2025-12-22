/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "recovery_mode_manager_base.hpp"

#include <string>

namespace nvidia::recovery
{

class USBRCMRecoveryManager : public RecoveryModeManagerBase
{
  public:
    USBRCMRecoveryManager(sdbusplus::bus_t& bus, std::string chassisName,
                          std::string objPath, std::string configType);

  protected:
    void performForceRecovery() override;

  private:
    /**
     * @brief Check if all devices are in recovery mode
     * @details Uses getRecoveryStatus() to enumerate USB devices and verify
     *          that ALL discovered devices report "In Recovery" status.
     *          This is important for C2G4 systems with multiple CPUs.
     * @return true if all devices report "In Recovery", false otherwise
     */
    bool areDevicesInRecovery() const;

    std::string configType;
};

} // namespace nvidia::recovery
