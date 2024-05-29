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


#include "base_item_updater.hpp"

#include "dbusutils.hpp"
#include "watch.hpp"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <cstring>
#include <experimental/any>
#include <filesystem>
#include <iostream>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

using namespace phosphor::logging;
namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;

int BaseItemUpdater::processImage(std::filesystem::path& filePath)
{
    // Compute id
    std::string uniqueIdentifier = filePath.parent_path().string();
    boost::replace_all(uniqueIdentifier, getImageUploadDir(), "");
    auto id = getIdProperty(uniqueIdentifier);
    if (id == "")
    {
        std::cerr << "\n Version ID not found ";
        return -1;
    }
    auto objPath = std::string{SOFTWARE_OBJPATH} + '/' + id;
    return initiateUpdateImage(objPath, filePath.string(), filePath.stem(), id,
                               uniqueIdentifier);
}

void BaseItemUpdater::erase(const std::string& versionId)
{
    auto it = versions.find(versionId);
    if (it == versions.end())
    {
        log<level::ERR>(("Error: Failed to find version " + versionId +
                         " in item updater versions map."
                         " Unable to remove.")
                            .c_str());
    }
    else
    {
        (*(it->second)).deleteObject.reset(nullptr);

        versions.erase(it);
    }
}

void BaseItemUpdater::readDeviceDetails(std::string& p)
{
    auto version = getVersion(p);
    createSoftwareObject(p, version);
    // Add matches for Device Inventory's property changes
    deviceMatches.emplace_back(
        bus, MatchRules::propertiesChanged(p, ITEM_IFACE),
        std::bind(&BaseItemUpdater::onInventoryChangedMsg, this,
                  std::placeholders::_1)); // For present
    deviceMatches.emplace_back(
        bus, MatchRules::propertiesChanged(p, ASSET_IFACE),
        std::bind(&BaseItemUpdater::onInventoryChangedMsg, this,
                  std::placeholders::_1)); // For model
}

void BaseItemUpdater::readExistingFirmWare()
{
    auto paths = getItemUpdaterInventoryPaths();
    for (auto p : paths)
    {
        readDeviceDetails(p);
    }
    // update the Existing firmwares
    if (std::filesystem::exists(IMG_DIR_PERSIST))
    {
        for (const auto& entry :
             std::filesystem::directory_iterator(IMG_DIR_PERSIST))
        {
            // Ignore return
            std::filesystem::path filePath = entry.path();
            std::error_code ec;
            if (std::filesystem::is_regular_file(filePath, ec))
            {
                processImage(filePath);
            }
            if (ec)
            {
                std::cerr << "Error in is_regular_file: " << ec.message();
            }
        }
    }
}

void BaseItemUpdater::createSoftwareObject(const std::string& inventoryPath,
                                           const std::string& deviceVersion)
{
    auto versionId = getIdProperty(deviceVersion);

    auto model = getModel(inventoryPath);
    auto manufacturer = getManufacturer(inventoryPath);
    auto objPath = std::string(SOFTWARE_OBJPATH) + "/" + versionId;

    auto it = versions.find(versionId);
    if (it != versions.end())
    {
        // version object already present
    }
    else
    {
        std::string uuid = getUUID(model, manufacturer);
        if (uuid == "")
        {
            std::cerr << "\nUUID not found " << uuid;
            return;
        }
        else
        {
            std::cerr << "\nUUID  found " << uuid << " " << model << " "
                      << manufacturer;
        }
        std::filesystem::path imageDirPath = getImageUploadDir();
        imageDirPath /= uuid;
        imageDirPath /= "na.img";
        auto status = Version::Status::Active;
        // Create a new object for running Device inventory
        auto versionPtr = createVersion(objPath, versionId, deviceVersion, uuid,
                                        imageDirPath.string(), status);
        // Add to Version list
        versions.insert(std::make_pair(versionId, std::move(versionPtr)));
    }
}

void BaseItemUpdater::onInventoryChangedMsg(sdbusplus::message::message& msg)
{
    using Interface = std::string;
    Interface interface;
    Properties properties;
    std::string devicePath = msg.get_path();

    msg.read(interface, properties);
    onInventoryChanged(devicePath, properties);
}

void BaseItemUpdater::onInventoryChanged(const std::string& devicePath,
                                         const Properties& properties)
{
    // Need to be checked
    std::optional<bool> present;
    std::optional<std::string> model;
    std::optional<std::string> manufacturer;

    auto p = properties.find(PRESENT);
    if (p != properties.end())
    {
        present = std::get<bool>(p->second);
        inventoryObjectStatusMap[devicePath].present = *present;
    }
    p = properties.find(MODEL);
    if (p != properties.end())
    {
        model = std::get<std::string>(p->second);
        inventoryObjectStatusMap[devicePath].model = *model;
    }
    p = properties.find(MANUFACTURER);
    if (p != properties.end())
    {
        manufacturer = std::get<std::string>(p->second);
        inventoryObjectStatusMap[devicePath].manufacturer = *manufacturer;
    }
    if (!present.has_value() && !model.has_value() && !manufacturer.has_value())
    {
        return;
    }

    if (inventoryObjectStatusMap[devicePath].present)
    {
        // If model is not updated, let's wait for it
        if (inventoryObjectStatusMap[devicePath].model.empty())
        {
            log<level::DEBUG>("Waiting for model to be updated");
            return;
        }
        // If manufacturer is not updated, let's wait for it
        if (inventoryObjectStatusMap[devicePath].manufacturer.empty())
        {
            log<level::DEBUG>("Waiting for manufacturer to be updated");
            return;
        }

        auto version = getVersion(devicePath);
        if (!version.empty())
        {
            createSoftwareObject(devicePath, version);
        }
        else
        {
            log<level::ERR>("Failed to get Device version",
                            entry("Device=%s", devicePath.c_str()));
        }
    }
    else
    {
        if (!present.has_value())
        {
            // Property not read wait for Present Status available
            return;
        }
    }
}
void BaseItemUpdater::invokeActivation(
    const std::unique_ptr<Version>& activation)
{

    activation->requestedActivation(Version::RequestedActivations::Active);
}

int BaseItemUpdater::initiateUpdateImage(const std::string& objPath,
                                         const std::string& filePath,
                                         const std::string& versionStr,
                                         const std::string& versionId,
                                         const std::string& uniqueIdentifier)
{
    // bool forceUpdate = true; // TODO get this from settings
    auto it = versions.find(versionId);
    if (it != versions.end())
    {
        if (it->second->activation() == Version::Status::Activating)
        {
            log<level::INFO>("Software activation is in progress",
                             entry("VERSION_ID=%s", versionId.c_str()));
            return -1;
        }
        erase(versionId); // remove
    }
    else
    {
        activationMatches.emplace_back(
            bus, MatchRules::propertiesChanged(objPath, ACTIVATE_INTERFACE),
            std::bind(&BaseItemUpdater::onReqActivationChangedMsg, this,
                      std::placeholders::_1));
    }
    // delete the activation interface and create again
    // so that PLDMD will identify
    auto status = Version::Status::Ready;
    // Create Version
    auto versionPtr =
        createVersion(objPath, versionId, versionStr, uniqueIdentifier,
                      filePath, status);

    versions.insert(std::make_pair(versionId, std::move(versionPtr)));
    return 0;
}

void BaseItemUpdater::onReqActivationChangedMsg(
    sdbusplus::message::message& msg)
{
    using Interface = std::string;
    Interface interface;
    Properties properties;
    std::string objPath = msg.get_path();

    msg.read(interface, properties);
    onReqActivationChanged(objPath, properties);
}

void BaseItemUpdater::onReqActivationChanged(const std::string& objPath,
                                             const Properties& properties)
{

    std::optional<std::string> reqActivation;
    auto p = properties.find("RequestedActivation");
    if (p != properties.end())
    {
        reqActivation = std::get<std::string>(p->second);
    }
    if (reqActivation.has_value())
    {
        if (NSActivation::convertRequestedActivationsFromString(
                *reqActivation) == NSActivation::RequestedActivations::Active)
        {
            std::string versionId = std::filesystem::path(objPath).filename();
            auto it = versions.find(versionId);
            if (it != versions.end())
            {
                (*(it->second))
                    .requestedActivation(Version::RequestedActivations::Active);
            }
        }
    }
}

std::unique_ptr<Version> BaseItemUpdater::createVersion(
    const std::string& objPath, const std::string& versionId,
    const std::string& versionString, const std::string& uniqueIdentifier,
    const std::string& filePath, const Version::Status& activationStatus)
{
    auto pair = deviceIds.find(uniqueIdentifier);
    std::string model = get<0>(pair->second);
    std::string manufacturer = get<1>(pair->second);
    auto versionPtr = std::make_unique<Version>(
        bus, versionString, objPath, uniqueIdentifier, versionId, filePath,
        activationStatus, model, manufacturer,
        std::bind(&BaseItemUpdater::erase, this, std::placeholders::_1), this,
        this);

    return versionPtr;
}
using DbusVariant = std::variant<std::string, bool, uint8_t, uint16_t, int16_t,
                                 uint32_t, int32_t, uint64_t, int64_t, double>;
void BaseItemUpdater::newDeviceAdded(sdbusplus::message::message& msg)
{
    try
    {
        sdbusplus::message::object_path objPath;
        std::map<std::string, std::map<std::string, DbusVariant>> interfaces;
        msg.read(objPath, interfaces);

        auto itIntf = interfaces.find(inventoryIface);
        if (itIntf != interfaces.cend())
        {
            if (pathIsValidDevice(objPath.str)) {
                readDeviceDetails(objPath.str);
            }
        }
    }
    catch (const std::exception& e)
    {
        // Ignore, the property may be of a different type than expected.
    }
}

TargetFilter BaseItemUpdater::applyTargetFilters(
    const std::vector<sdbusplus::message::object_path>& targets)
{
    TargetFilter targetFilter = {TargetFilterType::UpdateNone, {}};
    if (targets.size() == 0)
    {
        // update all
        targetFilter.type = TargetFilterType::UpdateAll;
        return targetFilter;
    }
    for (auto& target : targets)
    {
        std::string path = validateTarget(target);
        if (!path.empty())
        {
            targetFilter.type = TargetFilterType::UpdateSelected;
            targetFilter.targets.emplace_back(path);
        }
    }
    return targetFilter;
}
} // namespace updater
} // namespace software
} // namespace nvidia
