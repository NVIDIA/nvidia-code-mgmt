/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "vhmc_updater.hpp"

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>

namespace nvidia
{
namespace software
{
namespace updater
{

// Image links set up by vhmc_config_defaults.sh as VHMC_ACTIVE_IMAGE and
// VHMC_INACTIVE_IMAGE.
constexpr auto activeImageLink = "vhmc.squashfs";
constexpr auto inactiveImageLink = "inactive.squashfs";

// Slot ids are positional, so the running image is always reported as slot 0.
constexpr uint8_t activeSlotId = 0;
constexpr uint8_t inactiveSlotId = 1;

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

/** @brief Build the inventory object path published for the vHMC. */
static std::string firmwareInventoryPath()
{
    return std::string(VHMC_FIRMWARE_INVENTORY_OBJPATH) + "/" +
           VHMC_FIRMWARE_INVENTORY_ID;
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

void VHMCItemUpdater::publishFirmwareInventory(sdbusplus::bus_t& bus)
{
    const std::string inventoryId = VHMC_FIRMWARE_INVENTORY_ID;
    if (inventoryId.empty())
    {
        lg2::error("No vHMC firmware inventory id configured");
        return;
    }

    std::string objPath = firmwareInventoryPath();
    try
    {
        fwInventory = std::make_unique<VHMCFirmwareInventory>(bus, objPath);
        // The mapper turns this into SOFTWARE_OBJPATH/updateable for Redfish.
        fwInventory->associations({{"software_version", "updateable",
                                    VHMC_FIRMWARE_INVENTORY_OBJPATH}});
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to publish vHMC firmware inventory {PATH}: {ERROR}",
                   "PATH", objPath, "ERROR", std::string(e.what()));
    }
}

/** @brief Name of the metadata sidecar written alongside a slot image. */
static std::string metadataSidecarName(const std::filesystem::path& image)
{
    std::string name = image.filename().string();
    for (std::string_view suffix : {".squashfs-xz", ".squashfs"})
    {
        if (name.size() > suffix.size() && name.ends_with(suffix))
        {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name + ".json";
}

std::optional<VHMCItemUpdater::SlotMetadata>
    VHMCItemUpdater::readSlotMetadata(const std::string& imageLink) const
{
    std::error_code ec;
    std::filesystem::path image = imageLink;
    if (std::filesystem::is_symlink(image, ec))
    {
        auto target = std::filesystem::read_symlink(image, ec);
        if (ec)
        {
            lg2::error("Failed to resolve vHMC slot image {PATH}: {ERROR}",
                       "PATH", imageLink, "ERROR", ec.message());
            return std::nullopt;
        }
        image = target.is_absolute() ? target : image.parent_path() / target;
    }
    if (!std::filesystem::exists(image, ec))
    {
        return std::nullopt;
    }

    SlotMetadata metadata{};
    auto sidecar = image.parent_path() / metadataSidecarName(image);
    std::ifstream file(sidecar);
    if (!file.is_open())
    {
        lg2::info("No metadata for vHMC slot image {PATH}", "PATH",
                  image.string());
        return metadata;
    }

    try
    {
        auto data = nlohmann::json::parse(file);
        metadata.version = data.value("version", "");
        metadata.buildType = data.value("BuildType", "");

        auto stamp = data.find("ComponentComparisonStamp");
        if (stamp != data.end() && stamp->is_number_unsigned())
        {
            metadata.comparisonNumber = stamp->get<uint32_t>();
            metadata.hasComparisonNumber = true;
        }
        else if (stamp != data.end() && stamp->is_string())
        {
            metadata.comparisonNumber = static_cast<uint32_t>(
                std::stoul(stamp->get<std::string>(), nullptr, 0));
            metadata.hasComparisonNumber = true;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to read vHMC slot metadata {PATH}: {ERROR}", "PATH",
                   sidecar.string(), "ERROR", std::string(e.what()));
    }
    return metadata;
}

std::string
    VHMCItemUpdater::refreshSlot(std::unique_ptr<VHMCFirmwareSlot>& slot,
                                 uint8_t slotId, bool isActive,
                                 const std::string& imageLink)
{
    auto metadata = readSlotMetadata(imageLink);
    if (!metadata)
    {
        // Only the active image is always present, so an unpopulated slot is
        // normal; drop the object and report nothing for it.
        if (slot)
        {
            lg2::info("vHMC slot {ID} image {PATH} is gone, removing the slot",
                      "ID", slotId, "PATH", imageLink);
            slot.reset();
        }
        return "";
    }

    if (!slot)
    {
        std::string objPath =
            firmwareInventoryPath() + "/Slot" + std::to_string(slotId);
        try
        {
            slot = std::make_unique<VHMCFirmwareSlot>(slotBus, objPath);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to publish vHMC slot {PATH}: {ERROR}", "PATH",
                       objPath, "ERROR", std::string(e.what()));
            return "";
        }
        slot->slotId(slotId);
        slot->isActive(isActive);
        // The mapper turns this into <inventory>/ActiveSlot or
        // <inventory>/InactiveSlot, which is where Redfish looks for slots.
        slot->associations(
            {{"software", isActive ? "ActiveSlot" : "InactiveSlot",
              firmwareInventoryPath()}});
    }

    slot->extendedVersion(metadata->version);
    slot->state(isActive ? VHMCFirmwareSlot::FirmwareState::Activated
                         : VHMCFirmwareSlot::FirmwareState::Inactive);
    if (metadata->buildType == "Release")
    {
        slot->buildType(VHMCFirmwareSlot::FirmwareBuildType::Release);
    }
    else
    {
        // No vHMC image carries BuildType in its metadata yet, and every
        // image built so far is a development one.
        slot->buildType(VHMCFirmwareSlot::FirmwareBuildType::Development);
    }
    if (metadata->hasComparisonNumber)
    {
        slot->firmwareComparisonNumber(metadata->comparisonNumber);
    }
    return metadata->version;
}

void VHMCItemUpdater::refreshFirmwareSlots()
{
    if (slotWorkDir.empty())
    {
        return;
    }

    auto version = refreshSlot(activeSlot, activeSlotId, true,
                               slotWorkDir + activeImageLink);
    refreshSlot(inactiveSlot, inactiveSlotId, false,
                slotWorkDir + inactiveImageLink);

    // The inventory version is the one the vHMC is running from.
    if (fwInventory && !version.empty())
    {
        fwInventory->version(version);
    }
}

void VHMCItemUpdater::publishFirmwareSlots()
{
    slotWorkDir = getImageUploadDir();
    if (slotWorkDir.empty())
    {
        lg2::error("No vHMC work directory, not publishing firmware slots");
        return;
    }
    refreshFirmwareSlots();
    if (!activeSlot)
    {
        lg2::warning("No vHMC active image in {PATH}, no slots published yet",
                     "PATH", slotWorkDir);
    }
    startSlotWatch();
}

void VHMCItemUpdater::startSlotWatch()
{
    slotWatchFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (slotWatchFd < 0)
    {
        lg2::error("Failed to create vHMC slot watch: {ERRNO}", "ERRNO", errno);
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(slotWorkDir, ec);

    // Staging writes a metadata sidecar and a promotion replaces the image
    // links, so both file contents and directory entries have to be watched.
    if (inotify_add_watch(slotWatchFd, slotWorkDir.c_str(),
                          IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO |
                              IN_DELETE) < 0)
    {
        lg2::error("Failed to watch vHMC work directory {PATH}: {ERRNO}",
                   "PATH", slotWorkDir, "ERRNO", errno);
        close(slotWatchFd);
        slotWatchFd = -1;
        return;
    }

    // main() creates the default loop before the item updater is built and
    // keeps it alive for as long as the daemon runs.
    sd_event* loop = nullptr;
    auto rc = sd_event_default(&loop);
    if (rc >= 0)
    {
        rc = sd_event_add_io(loop, nullptr, slotWatchFd, EPOLLIN,
                             slotWatchCallback, this);
        sd_event_unref(loop);
    }
    if (rc < 0)
    {
        lg2::error("Failed to hook up vHMC slot watch: {RC}", "RC", rc);
        close(slotWatchFd);
        slotWatchFd = -1;
    }
}

int VHMCItemUpdater::slotWatchCallback([[maybe_unused]] sd_event_source* source,
                                       int fd, uint32_t revents, void* userdata)
{
    if (!(revents & EPOLLIN))
    {
        return 0;
    }

    // The events only report that something changed, so drain them and read
    // the slot state back from disk.
    alignas(struct inotify_event) std::array<char, 4096> buffer{};
    while (read(fd, buffer.data(), buffer.size()) > 0)
    {
    }

    auto* updater = static_cast<VHMCItemUpdater*>(userdata);
    if (updater != nullptr)
    {
        updater->refreshFirmwareSlots();
    }
    return 0;
}

VHMCItemUpdater::~VHMCItemUpdater()
{
    if (slotWatchFd >= 0)
    {
        close(slotWatchFd);
    }
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
