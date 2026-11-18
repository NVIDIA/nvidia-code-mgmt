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
#include <xyz/openbmc_project/Common/error.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

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

/** @brief Resolve a slot image link to the file it points at. */
static std::optional<std::filesystem::path>
    resolveSlotImageLink(const std::string& imageLink)
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
    return image;
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
    auto resolved = resolveSlotImageLink(imageLink);
    if (!resolved)
    {
        return std::nullopt;
    }
    const auto& image = *resolved;

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

void VHMCItemUpdater::publishHashCompute(sdbusplus::bus_t& bus)
{
    const std::string inventoryId = VHMC_FIRMWARE_INVENTORY_ID;
    if (inventoryId.empty())
    {
        return;
    }

    // Redfish derives the object name from the firmware inventory, so the
    // two have to agree.
    std::string objPath =
        std::string(VHMC_HASH_COMPUTE_OBJPATH) + "/" + inventoryId;
    try
    {
        hashCompute = std::make_unique<VHMCHashCompute>(
            bus, objPath,
            [this](uint16_t slotId) { return slotImagePath(slotId); });
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("Failed to publish vHMC digest provider {PATH}: {ERROR}",
                   "PATH", objPath, "ERROR", std::string(e.what()));
    }
}

std::string VHMCItemUpdater::slotImagePath(uint16_t slotId) const
{
    if (slotWorkDir.empty())
    {
        return "";
    }

    std::string imageLink;
    if (slotId == activeSlotId)
    {
        imageLink = slotWorkDir + activeImageLink;
    }
    else if (slotId == inactiveSlotId)
    {
        imageLink = slotWorkDir + inactiveImageLink;
    }
    else
    {
        return "";
    }

    auto image = resolveSlotImageLink(imageLink);
    return image ? image->string() : "";
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

// Hashing runs on the event loop the daemon serves D-Bus from, so it is
// chunked to keep the daemon responsive.
constexpr size_t hashChunkSize = 256 * 1024;

constexpr auto hashAlgorithm = "SHA384";

VHMCHashCompute::VHMCHashCompute(sdbusplus::bus_t& bus,
                                 const std::string& objPath,
                                 SlotImageResolver resolver) :
    hashComputeInherit(bus, objPath.c_str(), action::emit_interface_added),
    resolveSlotImage(std::move(resolver))
{
    algorithm(hashAlgorithm);
}

VHMCHashCompute::~VHMCHashCompute()
{
    releaseHash();
}

void VHMCHashCompute::getHash(uint16_t deviceId)
{
    using sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;
    using sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
    using sdbusplus::xyz::openbmc_project::Common::Error::Unavailable;

    if (digestCtx != nullptr)
    {
        lg2::error("A vHMC digest is already being computed");
        throw Unavailable();
    }

    std::string imagePath;
    if (resolveSlotImage)
    {
        imagePath = resolveSlotImage(deviceId);
    }
    if (imagePath.empty())
    {
        lg2::error("No vHMC image to hash for slot {ID}", "ID", deviceId);
        throw InvalidArgument();
    }

    image.open(imagePath, std::ios::binary);
    if (!image.is_open())
    {
        lg2::error("Failed to open vHMC image {PATH} for hashing", "PATH",
                   imagePath);
        releaseHash();
        throw InternalFailure();
    }

    digestCtx = EVP_MD_CTX_new();
    if (digestCtx == nullptr ||
        EVP_DigestInit_ex(digestCtx, EVP_sha384(), nullptr) != 1)
    {
        lg2::error("Failed to start the digest of vHMC image {PATH}", "PATH",
                   imagePath);
        releaseHash();
        throw InternalFailure();
    }
    chunk.resize(hashChunkSize);

    // Reset without emitting: an empty value on the bus reads as a failure.
    digest("", true);

    // A defer source left enabled runs once per loop iteration, so the bus
    // is served between chunks.
    sd_event* loop = nullptr;
    auto rc = sd_event_default(&loop);
    if (rc >= 0)
    {
        rc = sd_event_add_defer(loop, &hashSource, hashStepCallback, this);
        if (rc >= 0)
        {
            rc = sd_event_source_set_enabled(hashSource, SD_EVENT_ON);
        }
        sd_event_unref(loop);
    }
    if (rc < 0)
    {
        lg2::error("Failed to schedule the digest of vHMC image {PATH}: {RC}",
                   "PATH", imagePath, "RC", rc);
        failHash();
        throw InternalFailure();
    }

    lg2::info("Computing the {ALGORITHM} digest of vHMC image {PATH}",
              "ALGORITHM", std::string(hashAlgorithm), "PATH", imagePath);
}

int VHMCHashCompute::hashStepCallback([[maybe_unused]] sd_event_source* source,
                                      void* userdata)
{
    auto* self = static_cast<VHMCHashCompute*>(userdata);
    if (self == nullptr || self->digestCtx == nullptr)
    {
        return 0;
    }

    self->image.read(self->chunk.data(),
                     static_cast<std::streamsize>(self->chunk.size()));
    auto read = self->image.gcount();
    if (read > 0 && EVP_DigestUpdate(self->digestCtx, self->chunk.data(),
                                     static_cast<size_t>(read)) != 1)
    {
        lg2::error("Failed to hash a chunk of the vHMC image");
        self->failHash();
        return 0;
    }

    if (self->image.eof())
    {
        self->completeHash();
    }
    else if (!self->image.good())
    {
        lg2::error("Failed to read the vHMC image while hashing it");
        self->failHash();
    }
    return 0;
}

void VHMCHashCompute::completeHash()
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> value{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digestCtx, value.data(), &length) != 1)
    {
        lg2::error("Failed to finish the digest of the vHMC image");
        failHash();
        return;
    }

    std::string hex;
    hex.reserve(static_cast<size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i)
    {
        hex += std::format("{:02x}", value[i]);
    }

    releaseHash();
    digest(hex);
    lg2::info("Computed the vHMC image digest {DIGEST}", "DIGEST", hex);
}

void VHMCHashCompute::failHash()
{
    releaseHash();

    // Digest is already empty, and the generated setter only signals a
    // change, so the empty value has to be staged against a non-empty one.
    digest("failed", true);
    digest("");
}

void VHMCHashCompute::releaseHash()
{
    if (hashSource != nullptr)
    {
        sd_event_source_set_enabled(hashSource, SD_EVENT_OFF);
        sd_event_source_unref(hashSource);
        hashSource = nullptr;
    }
    if (digestCtx != nullptr)
    {
        EVP_MD_CTX_free(digestCtx);
        digestCtx = nullptr;
    }
    image.close();
    image.clear();
    chunk.clear();
    chunk.shrink_to_fit();
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
