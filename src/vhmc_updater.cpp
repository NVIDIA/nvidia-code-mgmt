/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "vhmc_updater.hpp"

#include <phosphor-logging/lg2.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>

namespace nvidia
{
namespace software
{
namespace updater
{

static std::string getVHMCWorkDirFromConfig()
{
    if (!std::filesystem::exists(VHMC_CONFIG_SCRIPT))
    {
        lg2::info("vHMC config script not found, using default path: {SCRIPT}",
                  "SCRIPT", std::string(VHMC_CONFIG_SCRIPT));
        return "";
    }
    std::array<char, 256> buffer;
    std::string result;
    std::string cmd = std::format(
        "bash -c '. \"{}\" 2>/dev/null; printf \"%s\" \"$VHMC_WORK_DIR\"'",
        VHMC_CONFIG_SCRIPT);
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
    {
        lg2::error("Failed to popen vHMC config script: {SCRIPT}", "SCRIPT",
                   std::string(VHMC_CONFIG_SCRIPT));
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' '))
    {
        result.pop_back();
    }
    if (result.empty())
    {
        lg2::warning(
            "vHMC config script returned empty VHMC_WORK_DIR: {SCRIPT}",
            "SCRIPT", std::string(VHMC_CONFIG_SCRIPT));
    }
    if (!result.empty() && result.back() != '/')
    {
        result += '/';
    }
    return result;
}

std::string VHMCItemUpdater::getImageUploadDir() const
{
    std::string dir = getVHMCWorkDirFromConfig();
    if (!dir.empty())
    {
        return dir;
    }
    auto fallback = IMG_UPLOAD_DIR_BASE + getName() + "/";
    lg2::info("Using default vHMC image upload dir: {PATH}", "PATH", fallback);
    return fallback;
}

std::string
    VHMCItemUpdater::validateTarget(const sdbusplus::object_path& target)
{
    const std::string name = target.filename();
    return name == VHMC_FIRMWARE_INVENTORY_ID ? name : "";
}

std::string VHMCItemUpdater::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string VHMCItemUpdater::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string VHMCItemUpdater::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

} // namespace updater
} // namespace software
} // namespace nvidia
