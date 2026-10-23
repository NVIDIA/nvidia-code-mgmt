/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <systemd/sd-event.h>

#include <sdbusplus/server/manager.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Software/BuildType/server.hpp>
#include <xyz/openbmc_project/Software/ExtendedVersion/server.hpp>
#include <xyz/openbmc_project/Software/Slot/server.hpp>
#include <xyz/openbmc_project/Software/State/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/Software/VersionComparison/server.hpp>

#include <memory>
#include <optional>
#include <string>

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
    sdbusplus::xyz::openbmc_project::Software::server::VersionComparison,
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
    VHMCFirmwareSlot(sdbusplus::bus::bus& bus, const std::string& objPath) :
        softwareSlotInherit(bus, objPath.c_str(), action::emit_interface_added)
    {}
};

using firmwareInventoryInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Version,
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
    VHMCFirmwareInventory(sdbusplus::bus::bus& bus,
                          const std::string& objPath) :
        firmwareInventoryInherit(bus, objPath.c_str(),
                                 action::emit_interface_added)
    {
        purpose(sdbusplus::xyz::openbmc_project::Software::server::Version::
                    VersionPurpose::Other);
    }
};

class VHMCItemUpdater : public BaseItemUpdater
{
  public:
    VHMCItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, VHMC_SUPPORTED_MODEL, VHMC_INVENTORY_IFACE, "VHMC",
                        VHMC_BUSNAME_UPDATER, VHMC_UPDATE_SERVICE, false,
                        VHMC_BUSNAME_INVENTORY),
        invObjManager(bus, VHMC_FIRMWARE_INVENTORY_OBJPATH), slotBus(bus)
    {
        publishFirmwareInventory(bus);
        publishFirmwareSlots();
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
    std::string
        validateTarget(const sdbusplus::message::object_path& target) override;

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
        uint32_t comparisonNumber = 0;
        bool hasComparisonNumber = false;
    };

    /** @brief Publish the firmware inventory object for the vHMC. */
    void publishFirmwareInventory(sdbusplus::bus::bus& bus);

    /** @brief Publish the vHMC image slots and watch them for changes. */
    void publishFirmwareSlots();

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

    /** @brief Bus the firmware inventory and slot objects live on. */
    sdbusplus::bus::bus& slotBus;

    /** @brief Directory holding the vHMC images and their metadata. */
    std::string slotWorkDir;

    /** @brief Firmware inventory object representing the vHMC. */
    std::unique_ptr<VHMCFirmwareInventory> fwInventory;

    /** @brief Slot holding the image the vHMC is running from. */
    std::unique_ptr<VHMCFirmwareSlot> activeSlot;

    /** @brief Slot holding the image staged for the next activation. */
    std::unique_ptr<VHMCFirmwareSlot> inactiveSlot;

    /** @brief inotify descriptor watching the vHMC work directory. */
    int slotWatchFd = -1;
};

} // namespace updater
} // namespace software
} // namespace nvidia
