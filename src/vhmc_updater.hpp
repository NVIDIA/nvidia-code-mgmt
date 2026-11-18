/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <openssl/evp.h>
#include <systemd/sd-event.h>

#include <com/nvidia/ComputeHash/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Software/BuildType/server.hpp>
#include <xyz/openbmc_project/Software/ExtendedVersion/server.hpp>
#include <xyz/openbmc_project/Software/Slot/server.hpp>
#include <xyz/openbmc_project/Software/State/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{

using softwareSlotInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Slot,
    sdbusplus::xyz::openbmc_project::Software::server::ExtendedVersion,
    sdbusplus::xyz::openbmc_project::Software::server::BuildType,
    sdbusplus::xyz::openbmc_project::Software::server::State,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

/**
 * @brief One slot of the vHMC A/B image pair on D-Bus.
 */
class VHMCFirmwareSlot : public softwareSlotInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     */
    VHMCFirmwareSlot(sdbusplus::bus_t& bus, const std::string& objPath) :
        softwareSlotInherit(bus, objPath.c_str(), action::emit_interface_added)
    {}
};

using firmwareInventoryInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Version,
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset,
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

/**
 * @brief The vHMC firmware inventory object on D-Bus.
 *
 * Not the shared SoftwareVersion, whose CPLD, PSU, MTD and switchtec users
 * must not pick up the updateable association.
 */
class VHMCFirmwareInventory : public firmwareInventoryInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     */
    VHMCFirmwareInventory(sdbusplus::bus_t& bus, const std::string& objPath) :
        firmwareInventoryInherit(bus, objPath.c_str(),
                                 action::emit_interface_added)
    {
        purpose(sdbusplus::xyz::openbmc_project::Software::server::Version::
                    VersionPurpose::Other);
        manufacturer(VHMC_FIRMWARE_MANUFACTURER);
        description(VHMC_FIRMWARE_DESCRIPTION);
        present(true);
    }
};

using hashComputeInherit = sdbusplus::server::object::object<
    sdbusplus::com::nvidia::server::ComputeHash>;

/**
 * @brief SHA384 digests of the vHMC images on D-Bus.
 */
class VHMCHashCompute : public hashComputeInherit
{
  public:
    /**
     * @brief Resolve a slot to the image file holding it.
     *
     * Returns an empty string when the slot is unknown or unpopulated.
     */
    using SlotImageResolver = std::function<std::string(uint16_t)>;

    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] resolver - Maps the requested slot to its image file
     */
    VHMCHashCompute(sdbusplus::bus_t& bus, const std::string& objPath,
                    SlotImageResolver resolver);

    /**
     * @brief Destructor
     */
    ~VHMCHashCompute() override;

    /**
     * @brief Start hashing the image held by a slot.
     *
     * The digest is computed in the background and published on the Digest
     * property, which is cleared first so that re-hashing an unchanged image
     * still produces a change for callers watching the property.
     *
     * @param[in] deviceId - Slot the digest is wanted for
     */
    void getHash(uint16_t deviceId) override;

  private:
    /**
     * @brief sd-event callback folding one chunk into the running digest.
     *
     * @param source Event source, unused
     * @param userdata Pointer to the VHMCHashCompute
     * @return 0, failures end the computation rather than the event loop
     */
    static int hashStepCallback(sd_event_source* source, void* userdata);

    /** @brief Publish the finished digest and release the computation. */
    void completeHash();

    /** @brief Publish an empty digest for a failure and release it. */
    void failHash();

    /** @brief Release the image, digest context and event source. */
    void releaseHash();

    /** @brief Maps the requested slot to the image file to hash. */
    SlotImageResolver resolveSlotImage;

    /** @brief Image being hashed. */
    std::ifstream image{};

    /** @brief Buffer holding the chunk being folded in. */
    std::vector<char> chunk{};

    /** @brief Running digest, also marks a computation as in progress. */
    EVP_MD_CTX* digestCtx = nullptr;

    /** @brief Event source driving the computation off the method call. */
    sd_event_source* hashSource = nullptr;
};

class VHMCItemUpdater : public BaseItemUpdater
{
  public:
    VHMCItemUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, VHMC_SUPPORTED_MODEL, VHMC_INVENTORY_IFACE, "VHMC",
                        VHMC_BUSNAME_UPDATER, VHMC_UPDATE_SERVICE, false,
                        VHMC_BUSNAME_INVENTORY),
        invObjManager(bus, VHMC_FIRMWARE_INVENTORY_OBJPATH),
        hashObjManager(bus, VHMC_HASH_COMPUTE_OBJPATH), slotBus(bus)
    {
        publishFirmwareInventory(bus);
        publishFirmwareSlots();
        publishHashCompute(bus);
    }

    /**
     * @brief Destructor
     */
    ~VHMCItemUpdater() override;

    std::string getImageUploadDir() const override;

    std::string getVersion(const std::string& inventoryPath) const override;
    std::string
        getManufacturer(const std::string& inventoryPath) const override;
    std::string getModel(const std::string& inventoryPath) const override;

    /**
     * @brief Validate that the update target is the vHMC BMC inventory.
     *
     * @param target D-Bus software object path from UpdatePolicy.Targets
     * @return Target basename if accepted, otherwise empty
     */
    std::string validateTarget(const sdbusplus::object_path& target) override;

    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath, const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {
        std::string args = inventoryPath;
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += version;
        std::string escaped;
        for (char c : args)
        {
            if (c == '-')
            {
                escaped += "_HYP_";
            }
            else if (c == '/')
            {
                escaped += '-';
            }
            else
            {
                escaped += c;
            }
        }
        return escaped;
    }

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        std::string invPath = std::string(SOFTWARE_OBJPATH) + "/VHMC";
        ret.emplace_back(invPath);
        return ret;
    }

    bool inventorySupported() override
    {
        return false;
    }

  private:
    /** @brief Metadata describing one vHMC image slot. */
    struct SlotMetadata
    {
        std::string version;
        std::string buildType;
    };

    /** @brief Publish the firmware inventory object for the vHMC. */
    void publishFirmwareInventory(sdbusplus::bus_t& bus);

    /** @brief Publish the vHMC image slots and watch them for changes. */
    void publishFirmwareSlots();

    /** @brief Publish the digest provider for the vHMC images. */
    void publishHashCompute(sdbusplus::bus_t& bus);

    /**
     * @brief Resolve a slot id to the image file the slot holds.
     *
     * @param slotId Slot identifier, as reported on the slot objects
     * @return Path of the image, empty when the slot is not populated
     */
    std::string slotImagePath(uint16_t slotId) const;

    /** @brief Refresh both slot objects from the on-disk image metadata. */
    void refreshFirmwareSlots();

    /**
     * @brief Refresh a single slot object from its image link.
     *
     * @param slot Slot object to create, update or drop
     * @param slotId Slot identifier reported on D-Bus
     * @param isActive Whether the slot holds the running image
     * @param imageLink Path of the symlink pointing at the slot image
     * @return Version read for the slot, empty when it did not resolve
     */
    std::string refreshSlot(std::unique_ptr<VHMCFirmwareSlot>& slot,
                            uint8_t slotId, bool isActive,
                            const std::string& imageLink);

    /**
     * @brief Read the metadata sidecar belonging to a slot image.
     *
     * @param imageLink Path of the symlink pointing at the slot image
     * @return Metadata of the slot, or nothing when the image is absent
     */
    std::optional<SlotMetadata>
        readSlotMetadata(const std::string& imageLink) const;

    /** @brief Watch the work directory so slot changes are picked up. */
    void startSlotWatch();

    /**
     * @brief sd-event callback draining the slot inotify descriptor.
     *
     * @param source Event source, unused
     * @param fd inotify descriptor
     * @param revents Events that matched for fd
     * @param userdata Pointer to the VHMCItemUpdater
     * @return 0 on success, -1 on failure
     */
    static int slotWatchCallback(sd_event_source* source, int fd,
                                 uint32_t revents, void* userdata);

    /** @brief Object manager covering the firmware inventory and its slots.
     *
     * The shared manager sits at SOFTWARE_OBJPATH, which is not an ancestor
     * of these objects, and sd-bus drops InterfacesAdded for a path no
     * manager covers.  Without this the object mapper only ever sees the
     * objects that existed when it introspected the service, so a slot
     * published after an update stays missing from Redfish.
     */
    sdbusplus::server::manager_t invObjManager;

    /** @brief Object manager covering the digest provider.
     *
     * The digest objects sit outside both of the other managed paths, so
     * they need a manager of their own for the same reason.
     */
    sdbusplus::server::manager_t hashObjManager;

    /** @brief Bus the firmware inventory and slot objects live on. */
    sdbusplus::bus_t& slotBus;

    /** @brief Directory holding the vHMC images and their metadata. */
    std::string slotWorkDir;

    /** @brief Firmware inventory object representing the vHMC. */
    std::unique_ptr<VHMCFirmwareInventory> fwInventory;

    /** @brief Slot holding the image the vHMC is running from. */
    std::unique_ptr<VHMCFirmwareSlot> activeSlot;

    /** @brief Slot holding the image staged for the next activation. */
    std::unique_ptr<VHMCFirmwareSlot> inactiveSlot;

    /** @brief Digest provider for the vHMC images. */
    std::unique_ptr<VHMCHashCompute> hashCompute;

    /** @brief inotify descriptor watching the vHMC work directory. */
    int slotWatchFd = -1;
};

} // namespace updater
} // namespace software
} // namespace nvidia
