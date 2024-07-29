/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "fstream"
#include <nlohmann/json.hpp>

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

  public:
    MTDItemUpdater(sdbusplus::bus::bus& bus, std::string mtdN, std::string modelName) :
		BaseItemUpdater(bus, modelName, MTD_INVENTORY_IFACE, "MTD_FW_" + mtdN,
						MTD_BUSNAME_UPDATER_BASE + mtdN,
                        MTD_UPDATE_SERVICE, false, MTD_BUSNAME_INVENTORY_BASE + mtdN),
		mtdName(mtdN)

    {
        std::string jsonPath = "/usr/share/mtd_targets/" + mtdName + ".json";

        try {
            if (std::filesystem::exists(jsonPath)) {
                std::ifstream jsonFile(jsonPath);

                if (!jsonFile.is_open()) {
                    std::cerr << "Could not open the file:" << jsonPath << std::endl;
                    return;
                }

                nlohmann::json mtdConfig;
                jsonFile >> mtdConfig;
                jsonFile.close();
                std::string inventory = mtdConfig["Inventory"];
                copyPath = mtdConfig["Path"];
                std::string off = mtdConfig["Offset"];
                versionOffset = static_cast<std::streamoff>(std::stoll(off, nullptr, 0));
                versionSize = mtdConfig["VersionSize"];
                auto objPath = std::string(SOFTWARE_OBJPATH) + "/" + inventory;
                softwareVersionObj = std::make_unique<SoftwareVersion>(bus, objPath);
                getVersion("");
            }
            else {
                std::cerr << "Json file:" << jsonPath << " not found. Will not host fw inventory object" << std::endl;
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            std::cerr << "Failed to process the file:" << jsonPath << std::endl;
        }
    }

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion([
		[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getManufacturer([
        [maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel([
        [maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string
        getServiceArgs([[maybe_unused]] const std::string& inventoryPath,
                       const std::string& imagePath,
                       [[maybe_unused]] const std::string& version,
                       [[maybe_unused]] const TargetFilter &targetFilter) const override
    {
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += mtdName;
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        std::string invPath =
            std::string(SOFTWARE_OBJPATH) + "/" + mtdName;
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
    void cleanupImageUploadDir(const std::filesystem::path& path, Version* version) const override {
        BaseItemUpdater::cleanupImageUploadDir(path, version);
        getVersion("");
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
