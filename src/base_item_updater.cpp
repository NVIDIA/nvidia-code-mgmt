
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

int BaseItemUpdater::processImage(std::filesystem::path& filePath)
{
    // Compute id
    auto id = createVersionID(getName(), filePath.stem());

    auto objPath = std::string{SOFTWARE_OBJPATH} + '/' + id;

    std::string uniqueIdentifier = filePath.parent_path().string();
    boost::replace_all(uniqueIdentifier, getImageUploadDir(), "");

    return initiateUpdateImage(objPath, filePath.string(), filePath.stem(), id,
                               uniqueIdentifier);
}

void BaseItemUpdater::removeObject(const std::string& inventoryPath)
{
    for (auto it = versions.begin(); it != versions.end(); ++it)
    {
        const auto& versionPtr = it->second;

        auto associations = versionPtr->associations();
        for (auto iter = associations.begin(); iter != associations.end();)
        {
            if ((std::get<2>(*iter)).compare(inventoryPath) == 0)
            {
                iter = associations.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
        auto endpoints = versionPtr->endpoints();
        for (auto iter = endpoints.begin(); iter != endpoints.end();)
        {
            if (*iter == inventoryPath)
            {
                iter = endpoints.erase(iter);
            }
            else
            {
                ++iter;
            }
        }

        if (associations.empty() || endpoints.empty())
        {
            // Remove the activation
            erase(versionPtr->getVersionId());
        }
        else
        {
            // Update association
            versionPtr->associations(associations);
            // Update endpoints
            versionPtr->endpoints(endpoints);
        }
    }
}

void BaseItemUpdater::onUpdateDone(const std::string& versionId,
                                   const std::string& inventoryPath)
{

    // After update is done, remove old activation objects
    // Remove association from Old associations
    for (auto it = versions.begin(); it != versions.end(); ++it)
    {
        if (it->second->getVersionId() != versionId &&
            isAssociated(inventoryPath, it->second->associations()))
        {
            // remove Migrateed associations from other Versions
            const auto& versionPtr = it->second;

            auto associations = versionPtr->associations();
            auto ignoreCout = 0;
            for (auto iter = associations.begin(); iter != associations.end();)
            {
                if ((std::get<0>(*iter)).compare(ACTIVE_FWD_ASSOCIATION) == 0 ||
                    (std::get<0>(*iter)).compare(FUNCTIONAL_FWD_ASSOCIATION) ==
                        0 ||
                    (std::get<0>(*iter)).compare(UPDATEABLE_FWD_ASSOCIATION) ==
                        0)
                {
                    ignoreCout++;
                }
                if ((std::get<2>(*iter)).compare(inventoryPath) == 0)
                {
                    iter = associations.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
            auto assocSize = static_cast<int>(associations.size());
            if (associations.empty() || assocSize <= ignoreCout)
            {
                // Remove the activation
                erase(it->second->getVersionId());
            }
            else
            {
                // Update association
                versionPtr->associations(associations);
            }
            break;
        }
    }
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
    auto service = getService(p.c_str(), ITEM_IFACE);
    auto present =
        getProperty<bool>(service.c_str(), p.c_str(), ITEM_IFACE, PRESENT);
    auto version = getVersion(p);
    if (present && !version.empty())
    {
        createSoftwareObject(p, version);
    }
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
    auto versionId = createVersionID(getName(), deviceVersion);
    auto model = getModel(inventoryPath);
    auto manufacturer = getManufacturer(inventoryPath);
    auto objPath = std::string(SOFTWARE_OBJPATH) + "/" + versionId;

    auto it = versions.find(versionId);
    if (it != versions.end())
    {
        // The versionId is already created, associate the path
        auto associations = it->second->associations();
        associations.emplace_back(std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                                  ACTIVATION_REV_ASSOCIATION,
                                                  inventoryPath));
        it->second->associations(associations);

        auto eps = it->second->endpoints();
        eps.emplace_back(sdbusplus::message::object_path(inventoryPath));
        it->second->endpoints(eps);
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

        AssociationList associations;
        associations.emplace_back(std::make_tuple(ACTIVATION_FWD_ASSOCIATION,
                                                  ACTIVATION_REV_ASSOCIATION,
                                                  inventoryPath));
        // Create a new object for running Device inventory
        auto versionPtr =
            createVersion(objPath, versionId, deviceVersion, uuid,
                          imageDirPath.string(), associations, status);

        versionPtr->createActiveAssociation(objPath);
        versionPtr->addFunctionalAssociation(objPath);
        versionPtr->addUpdateableAssociation(objPath);

        auto eps = versionPtr->endpoints();
        eps.emplace_back(sdbusplus::message::object_path(inventoryPath));
        versionPtr->endpoints(eps);
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
            // remove the association from other Versions
            // Check if there is new  images to update
            // New image ?
        }
        else
        {
            // TODO: log an event
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
        // Remove object or association
        removeObject(devicePath);
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
    bool forceUpdate = true; // TODO get this from settings
    auto it = versions.find(versionId);
    AssociationList associations;
    if (it != versions.end())
    {
        if (it->second->activation() == Version::Status::Activating)
        {
            log<level::INFO>("Software activation is in progress",
                             entry("VERSION_ID=%s", versionId.c_str()));
            return -1;
        }

        if (forceUpdate == false)
        {
            associations = it->second->associations();
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
                      filePath, associations, status);

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
    const std::string& filePath, const AssociationList& assoc,
    const Version::Status& activationStatus)
{
    auto pair = deviceIds.find(uniqueIdentifier);
    std::string model = (pair->second).first;
    std::string manufacturer = (pair->second).second;
    auto versionPtr = std::make_unique<Version>(
        bus, versionString, objPath, uniqueIdentifier, versionId, filePath,
        assoc, activationStatus, model, manufacturer,
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
            readDeviceDetails(objPath.str);
        }
    }
    catch (const std::exception& e)
    {
        // Ignore, the property may be of a different type than expected.
    }
}

} // namespace updater
} // namespace software
} // namespace nvidia
