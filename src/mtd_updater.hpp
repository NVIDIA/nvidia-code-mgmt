/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <nlohmann/json.hpp>

#include "fstream"

// Add the Asset interface definition
using AssetObject = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset>;

namespace nvidia
{
namespace software
{
namespace updater
{

class MTDItemUpdater : public BaseItemUpdater
{
    std::string mtdName;
    std::string copyPath;
    std::streamoff versionOffset;
    std::size_t versionSize;
    std::unique_ptr<SoftwareVersion> softwareVersionObj;
    std::unique_ptr<SoftwareSettings> softwareSettingsObj;
    std::string inventory;
    std::string manufacturer;
    std::unique_ptr<AssetObject> assetObject;
    bool nostrip;

  public:
    MTDItemUpdater(sdbusplus::bus::bus& bus, std::string mtdN,
                   std::string modelName, bool nostrip) :
        BaseItemUpdater(bus, modelName, MTD_INVENTORY_IFACE,
                        computeInventory(mtdN), MTD_BUSNAME_UPDATER_BASE + mtdN,
                        MTD_UPDATE_SERVICE, false,
                        MTD_BUSNAME_INVENTORY_BASE + mtdN),
        mtdName(mtdN), nostrip(nostrip)

    {
        std::string jsonPath = "/usr/share/mtd_targets/" + mtdName + ".json";

        try
        {
            if (std::filesystem::exists(jsonPath))
            {
                std::ifstream jsonFile(jsonPath);

                if (!jsonFile.is_open())
                {
                    std::cerr << "Could not open the file:" << jsonPath
                              << std::endl;
                    return;
                }

                nlohmann::json mtdConfig;
                jsonFile >> mtdConfig;
                jsonFile.close();
                inventory = mtdConfig["Inventory"];
                copyPath = mtdConfig["Path"];
                std::string off = mtdConfig["Offset"];
                versionOffset =
                    static_cast<std::streamoff>(std::stoll(off, nullptr, 0));
                versionSize = mtdConfig["VersionSize"];

                // Check if Manufacturer exists and is not null
                if (mtdConfig.contains("Manufacturer") &&
                    !mtdConfig["Manufacturer"].is_null())
                {
                    manufacturer = mtdConfig["Manufacturer"];
                }
                else
                {
                    // Default manufacturer if not specified or null
                    manufacturer = "NVIDIA";
                    std::cerr
                        << "Manufacturer not specified in config, using default: "
                        << manufacturer << std::endl;
                }

                auto objPath = std::string(SOFTWARE_OBJPATH) + "/" + inventory;
                softwareVersionObj =
                    std::make_unique<SoftwareVersion>(bus, objPath);
                getVersion("");
                softwareSettingsObj =
                    std::make_unique<SoftwareSettings>(bus, objPath);

                // Create Asset object with Manufacturer property
                std::cerr << "Creating Asset object at path: " << objPath
                          << " with Manufacturer: " << manufacturer
                          << std::endl;

                // Create the Asset interface using AssetObject with the correct
                // constructor
                assetObject =
                    std::make_unique<AssetObject>(bus, objPath.c_str());

                // Set the properties after creation
                if (assetObject)
                {
                    assetObject->manufacturer(manufacturer);
                    std::cerr
                        << "Asset object created successfully with manufacturer: "
                        << manufacturer << std::endl;
                }
                else
                {
                    std::cerr << "Failed to create Asset object" << std::endl;
                }

#ifdef VERIFY_PCIECHIP
                bool checkSignature = mtdConfig["Authentication"];
                if (checkSignature)
                {
                    lg2::info("{TGT}: Image Authentication is Enabled", "TGT",
                              mtdName);
                    publicKey = PUBKEY_PCIECHIP;
                    if (publicKey.empty())
                    {
                        lg2::error(
                            "Public key is empty. Please set PUBKEY_PCIECHIP");
                    }
                }
                else
                {
                    lg2::info("{TGT}: Image Authentication is Disabled", "TGT",
                              mtdName);
                }
#endif
            }
            else
            {
                std::cerr << "Json file:" << jsonPath
                          << " not found. Will not host fw inventory object"
                          << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << std::endl;
            std::cerr << "Failed to process the file:" << jsonPath << std::endl;
        }
    }
    /**
     * @brief compute the inventory name for the correct update messaging
     *
     * @param std::string mtd name
     * @return std::string inventory value
     */
    static std::string computeInventory(const std::string& mtdN)
    {
        std::string jsonPath = "/usr/share/mtd_targets/" + mtdN + ".json";

        if (std::filesystem::exists(jsonPath))
        {
            try
            {
                std::ifstream jsonFile(jsonPath);
                if (jsonFile.is_open())
                {
                    nlohmann::json mtdConfig;
                    jsonFile >> mtdConfig;
                    return mtdConfig["Inventory"];
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error reading inventory from JSON: " << e.what()
                          << std::endl;
            }
        }

        // Default case if the file doesn't exist or reading fails
        return "MTD_FW_" + mtdN;
    }
    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getManufacturer(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override;

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        std::string invPath = std::string(SOFTWARE_OBJPATH) + "/" + mtdName;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout from config file for retimer
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return MTD_UPDATE_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for mtd inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief method to clean up image dirs. Use this method to update
     * fw inventory version
     */
    void cleanupImageUploadDir(const std::filesystem::path& path,
                               Version* version) const override
    {
        BaseItemUpdater::cleanupImageUploadDir(path, version);
        getVersion("");
    }

    virtual bool needVerify() const override
    {
        return !publicKey.empty() &&
               std::filesystem::exists("/var/check_signature");
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
