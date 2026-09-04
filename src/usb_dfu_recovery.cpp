// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "config.h"

#include "usb_dfu_recovery.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

std::string USBDFURecovery::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string USBDFURecovery::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "Nvidia";
}

std::string USBDFURecovery::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "USB DFU Recovery";
}

} // namespace updater
} // namespace software
} // namespace nvidia
