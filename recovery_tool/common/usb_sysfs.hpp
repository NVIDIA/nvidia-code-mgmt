// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
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

/** A USB device found in sysfs. */
struct Device
{
    /** Directory name = kernel port path, e.g. "1-1.3" */
    std::string portPath;
    std::string vid;
    std::string pid;
};

/**
 * Find the first device directory directly under `root` whose idVendor /
 * idProduct match `vid` / `pid` (lower-case hex without "0x", the sysfs
 * representation).  If `portFilter` is non-empty only the directory with that
 * exact name (kernel port path) is considered.  Hub/interface entries without
 * idVendor are skipped.  Never throws; an unreadable root yields nullopt.
 */
inline std::optional<Device>
    findDevice(const std::filesystem::path& root, std::string_view vid,
               std::string_view pid, std::string_view portFilter = {}) noexcept
{
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root, ec),
              end = std::filesystem::directory_iterator();
         !ec && it != end; it.increment(ec))
    {
        const auto& dev = it->path();
        if (!portFilter.empty() && dev.filename().string() != portFilter)
        {
            continue;
        }
        const auto devVid = readAttribute(dev / "idVendor");
        if (devVid.empty() || devVid != vid)
        {
            continue;
        }
        const auto devPid = readAttribute(dev / "idProduct");
        if (devPid == pid)
        {
            return Device{dev.filename().string(), devVid, devPid};
        }
    }
    return std::nullopt;
}

} // namespace usb_sysfs
