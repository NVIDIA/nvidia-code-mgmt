/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "hal/i_device.hpp"

namespace pwrseq::hal
{

std::string_view to_string(Result r) noexcept
{
    switch (r)
    {
        case Result::Ok:
            return "Ok";
        case Result::NotPresent:
            return "NotPresent";
        case Result::Absent:
            return "Absent";
        case Result::AlreadyInitialized:
            return "AlreadyInitialized";
        case Result::InitFailed:
            return "InitFailed";
        case Result::NotReady:
            return "NotReady";
        case Result::HwError:
            return "HwError";
        case Result::BadArgument:
            return "BadArgument";
        case Result::NotSupported:
            return "NotSupported";
    }
    return "?";
}

std::string DeviceConfig::param(std::string_view key) const noexcept
{
    for (const auto& [k, v] : driverParams)
    {
        if (k == key)
            return v;
    }
    return {};
}

} // namespace pwrseq::hal
