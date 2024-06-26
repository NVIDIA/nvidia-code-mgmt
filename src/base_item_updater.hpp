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

#include "activation_listener.hpp"
#include "dbusutils.hpp"
#include "version.hpp"

#include <sdbusplus/server.hpp>

#include <filesystem>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{
namespace Server = sdbusplus::xyz::openbmc_project::Software::server;
using NSActivation = Server::Activation;

/**
 * This an ADT for Item Updaters
 *  1) Abstracts underlying device specific implementations
 *  2) Performs Updates
 *  3) Creates and updates dbus object for update
 */
class BaseItemUpdater :
    public DBUSUtils,
    public ActivationListener,
    public ItemUpdaterUtils
{
  public:
    /**
     * @brief Constructor
     * @param bus
     * @param supportedDevices
     * @param inventoryIface
     * @param name
     * @param busName
     * @param serviceName
     * @param updateTogether
     */
    BaseItemUpdater(sdbusplus::bus::bus& bus,
                    const std::string& supportedDevices,
                    const std::string& inventoryIface, const std::string& name,
                    const std::string& busName, const std::string& serviceName,
                    bool updateTogether, const std::string& inventoryBusName) :
        DBUSUtils(bus),
        _name(name), busName(busName), serviceName(serviceName),
        inventoryIface(inventoryIface), updateTogether(updateTogether),
        inventoryBusName(inventoryBusName)
    {
        // supportedDevices
        std::vector<std::string> supportedModels;
        std::vector<std::filesystem::path> pathsToMonitor;
        boost::split(supportedModels, supportedDevices, boost::is_any_of("|"));
        if (supportedModels.size())
        {
            for (const std::string& sm : supportedModels)
            {
                std::vector<std::string> manufactureModel;
                boost::split(manufactureModel, sm, boost::is_any_of(":"));
                /* expected string format <Manufacture>:<Model>:<UUID> */
                if (manufactureModel.size() == 3)
                {
                    insertToUUIDMap(manufactureModel[2], manufactureModel[1],
                                    manufactureModel[0]);
                }
            }
        }
    }
    /**
     * @brief Destructor
     */
    virtual ~BaseItemUpdater()
    {}

    /**
     * @brief Get the Name object
     *
     * @return std::string
     */
    virtual std::string getName() const
    {
        return _name;
    }

    /**
     * @brief Intiates the image update
     *
     * @param objPath
     * @param filePath
     * @param versionId
     * @param versionStr
     * @param uniqueIdentifier
     * @return int
     */
    virtual int initiateUpdateImage(const std::string& objPath,
                                    const std::string& filePath,
                                    const std::string& versionId,
                                    const std::string& versionStr,
                                    const std::string& uniqueIdentifier);

    /**
     * @brief Create a Version object
     *
     * @param objPath
     * @param versionId
     * @param versionString
     * @param uniqueIdentifier
     * @param filePath
     * @param activationStatus
     * @return std::unique_ptr<Version>
     */
    std::unique_ptr<Version> createVersion(
        const std::string& objPath, const std::string& versionId,
        const std::string& versionString, const std::string& uniqueIdentifier,
        const std::string& filePath, const Version::Status& activationStatus);

    /**
     * @brief Get the Image Upload Dir object
     *
     * @return std::string
     */
    virtual std::string getImageUploadDir() const
    {
        return IMG_UPLOAD_DIR_BASE + getName() + "/";
    }
    /**
     * @brief Get the Bus Name object
     *
     * @return std::string
     */
    virtual std::string getBusName() const
    {
        return busName;
    }
    /**
     * @brief Get the Service Name object
     *
     * @return std::string
     */
    virtual std::string getServiceName() const
    {
        return serviceName;
    }
    /**
     * @brief Indicates wheather to do update all together at once
     *
     * @return true
     * @return false
     */
    virtual bool updateAllTogether() const
    {
        return updateTogether;
    }
    /**
     * @brief Get the Paths To Monitor object
     *
     * @return std::vector<std::filesystem::path>
     */
    virtual std::vector<std::filesystem::path> getPathsToMonitor() const
    {
        std::vector<std::filesystem::path> pathsToMonitor;
        for (const auto& sm : deviceIds)
        {
            std::filesystem::path pathToWatch(getImageUploadDir());
            pathToWatch /= sm.first;
            pathsToMonitor.push_back(pathToWatch);
        }
        if (pathsToMonitor.size() < 1)
        {
            // Fail
            throw std::runtime_error("No " + getName() + " to monitor");
        }
        return pathsToMonitor;
    }
    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    virtual std::vector<std::string> getItemUpdaterInventoryPaths()
    {
        std::vector<std::string> ret;
        try
        {
            auto paths = getinventoryPath(inventoryIface);
            for (auto p : paths)
            {
                if (pathIsValidDevice(p))
                {
                    ret.push_back(p);
                }
            }
        }
        catch (...)
        {}
        return ret;
    }

    /**
     * @brief inserts into map which contains UUid to model-manufacture hash
     *
     * @param uuid
     * @param model
     * @param manufacture
     */
    virtual void insertToUUIDMap(const std::string& uuid,
                                 const std::string& model,
                                 const std::string& manufacture)
    {

        auto pair =
            std::make_tuple(std::move(model), std::move(manufacture), "");
        auto npair = std::make_pair(std::move(uuid), pair);
        deviceIds.insert(npair);
    }
    /**
     * @brief for a given Model and manufacture gets UUID
     *
     * @param model
     * @param manufacture
     * @return std::string
     */
    virtual std::string getUUID(const std::string& model,
                                const std::string& manufacture)
    {
        std::string uuid = "";
        for (auto& it : deviceIds)
        {
            if (uuid.empty())
            {
                // use first uuid as default
                // this is to allow update even if
                // model mismatch from gpumgr occurs
                uuid = it.first;
            }
            auto& pair = it.second;
            if (get<0>(pair) == model && get<1>(pair) == manufacture)
            {
                uuid = it.first;
            }
        }
        return uuid;
    }

    /**
     * @brief Process images,
     *          1) Creates Unique ID for dbus object
     *          2) calls initiates update
     *
     * @param filePath
     * @return int
     */
    virtual int processImage(std::filesystem::path& filePath);

    /**
     * @brief removes inventory path from version dbus object
     *
     * @param inventoryPath
     */
    void removeObject(const std::string& inventoryPath);

    /**
     * @brief Reads existing firmware and updates the devices
     *
     */
    void readExistingFirmWare();

    /**
     * @brief deletes version from dbus
     *
     * @param versionId
     */
    void erase(const std::string& versionId);

    /**
     * @brief Create a Software Object object
     *
     * @param inventoryPath
     * @param deviceVersion
     */
    void createSoftwareObject(const std::string& inventoryPath,
                              const std::string& deviceVersion);

    /**
     * @brief When the inventory of monitored device changes the status of the
     * object is updated in version object
     *
     * @param msg
     */
    void onInventoryChangedMsg(sdbusplus::message::message& msg);

    /**
     * @brief When the inventory of monitored device changes the status of the
     * object is updated in version object
     *
     * @param devicePath
     * @param properties
     */
    void onInventoryChanged(const std::string& devicePath,
                            const Properties& properties);

    /**
     * @brief On onvoke of activation the f/w update scripts / commands are
     * invoked
     *
     * @param activation
     */
    void invokeActivation(const std::unique_ptr<Version>& activation);

    // TODO add VDT methods here

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    virtual std::string getVersion(const std::string& inventoryPath) const = 0;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    virtual std::string
        getManufacturer(const std::string& inventoryPath) const = 0;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    virtual std::string getModel(const std::string& inventoryPath) const = 0;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @return std::string
     */
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath, const std::string& version,
                       const TargetFilter& targetFilter) const = 0;
    /**
     * @brief Call back method when dbus activation change signal is received
     *
     * @param msg
     */
    void onReqActivationChangedMsg(sdbusplus::message::message& msg);

    /**
     * @brief Call back method when dbus activation change signal is received
     *
     * @param objPath
     * @param properties
     */
    void onReqActivationChanged(const std::string& objPath,
                                const Properties& properties);

    /**
     * @brief Get the Update Service With Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getUpdateServiceWithArgs(
        const std::string& inventoryPath, const std::string& imagePath,
        const std::string& version, const TargetFilter& targetFilter) const
    {
        auto args =
            getServiceArgs(inventoryPath, imagePath, version, targetFilter);
        auto service = getServiceName();
        auto p = service.find('@');
        assert(p != std::string::npos);
        service.insert(p + 1, args);
        return service;
    }

    /**
     * @brief Get Dbus service name from mapper. Override this
     *        method if implementation knows the dbus service
     *        name and read it from config file.
     * @param path
     * @param interface
     * @return std::string
     */
    std::string getDbusService(const std::string& path,
                               const std::string& interface)
    {
        return getService(path.c_str(), interface.c_str());
    }
    /**
     * @brief call back for new device add dbus signal
     *
     * @param msg
     */
    void newDeviceAdded(sdbusplus::message::message& msg);

    /**
     * @brief Processing new device addition
     *
     */
    virtual void watchNewlyAddedDevice()
    {
        deviceIfacesAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            sdbusplus::bus::match::rules::interfacesAdded() +
                sdbusplus::bus::match::rules::sender(inventoryBusName),
            std::bind(&BaseItemUpdater::newDeviceAdded, this,
                      std::placeholders::_1));
    }
    /**
     * @brief read the f/w details of the device
     *
     * @param p device dbus path
     */
    virtual void readDeviceDetails(std::string& p);

    /**
     * @brief Verifies that the path is a valid device
     *
     * @param p path to validate
     * @return true if valid
     * @return false if not valid
     */
    virtual bool pathIsValidDevice(std::string& p)
    {
        (void)p;
        return true;
    }

    /**
     * @brief validate non-pldm target object path
     *
     * @param target
     * @return std::string
     */
    virtual std::string
        validateTarget(const sdbusplus::message::object_path& target)
    {
        return target.filename();
    }

    /**
     * @brief to get propertyID
     *
     * @param string
     * @return std::string
     */
    virtual std::string getIdProperty(const std::string&)
    {
        return getName();
    }

    /**
     * @brief apply target filters for non-pldm devices
     *
     * @param targets
     * @return TargetFilter
     */
    TargetFilter applyTargetFilters(
        const std::vector<sdbusplus::message::object_path>& targets);

    /**
     * @brief Get timeout in seconds. Device implmenetation should override this
     *        value based on actual time required for the particular device.
     *
     * @return uint32_t
     */
    virtual uint32_t getTimeout()
    {
        return NON_PLDM_DEFAULT_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return true - if inventory is supported else false
     */
    virtual bool inventorySupported()
    {
        return true; // default is supported
    }

  protected:
    std::string _name;

    struct inventoryObjectStatus
    {
        bool present;
        std::string model;
        std::string manufacturer;
    };

    std::map<std::string, inventoryObjectStatus> inventoryObjectStatusMap;

    std::map<std::string, std::unique_ptr<Version>> versions;

    std::vector<sdbusplus::bus::match_t> deviceMatches;

    std::map<std::string, std::tuple<std::string, std::string, std::string>>
        deviceIds;

    std::string imageUploadDir;
    std::string busName;
    std::string serviceName;
    std::vector<sdbusplus::bus::match_t> activationMatches;
    std::string inventoryIface;
    bool updateTogether;
    std::unique_ptr<sdbusplus::bus::match_t> deviceIfacesAddedMatch;
    std::string inventoryBusName;
};

} // namespace updater
} // namespace software
} // namespace nvidia
