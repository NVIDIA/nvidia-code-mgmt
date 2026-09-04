// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

/**
 * Minimal, exception-free helpers for inspecting USB devices through sysfs
 * (/sys/bus/usb/devices).  Kept header-only so recovery tools that must stay
 * free of libusb / D-Bus dependencies can share it.
 */
namespace usb_sysfs
{

/**
 * Read a single-line sysfs attribute, trimmed and lower-cased.
 * Returns an empty string if the attribute cannot be read.
 */
inline std::string readAttribute(const std::filesystem::path& attr) noexcept
{
    try
    {
        std::ifstream f(attr);
        if (!f.is_open())
        {
            return {};
        }
        std::string value;
        std::getline(f, value);
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.pop_back();
        }
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return value;
    }
    catch (...)
    {
        return {};
    }
}

/**
 * Return true if any device directory directly under `root` reports
 * idVendor == vid and idProduct == pid.  Both IDs must be given as lower-case
 * hex without a "0x" prefix (the sysfs representation, e.g. "2245", "2700").
 * Hub/interface entries without idVendor are skipped.  Never throws; an
 * unreadable root simply yields false.
 */
inline bool hasDeviceWithVidPid(const std::filesystem::path& root,
                                std::string_view vid,
                                std::string_view pid) noexcept
{
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root, ec),
              end = std::filesystem::directory_iterator();
         !ec && it != end; it.increment(ec))
    {
        const auto& dev = it->path();
        if (readAttribute(dev / "idVendor") != vid)
        {
            continue;
        }
        if (readAttribute(dev / "idProduct") == pid)
        {
            return true;
        }
    }
    return false;
}

} // namespace usb_sysfs
