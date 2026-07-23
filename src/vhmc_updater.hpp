/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "config.h"

#include "base_item_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

class VHMCItemUpdater : public BaseItemUpdater
{
  public:
    VHMCItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, VHMC_SUPPORTED_MODEL, VHMC_INVENTORY_IFACE, "VHMC",
                        VHMC_BUSNAME_UPDATER, VHMC_UPDATE_SERVICE, false,
                        VHMC_BUSNAME_INVENTORY)
    {}

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
};

} // namespace updater
} // namespace software
} // namespace nvidia
