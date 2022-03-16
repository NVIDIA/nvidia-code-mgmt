#pragma once
#include "config.h"

#include "activation_listener.hpp"
#include "association_interface.hpp"
#include "dbusutils.hpp"
#include "version.hpp"

#include <filesystem>
#include <sdbusplus/server.hpp>
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
class BaseItemUpdater : public DBUSUtils,
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
                        const std::string& inventoryIface,
                        const std::string& name, const std::string& busName,
                        const std::string& serviceName, bool updateTogether,
                        const std::string& inventoryBusName) :
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
    {
    }

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
     * @param assoc
     * @param activationStatus
     * @return std::unique_ptr<Version>
     */
    std::unique_ptr<Version>
        createVersion(const std::string& objPath, const std::string& versionId,
                      const std::string& versionString,
                      const std::string& uniqueIdentifier,
                      const std::string& filePath, const AssociationList& assoc,
                      const Version::Status& activationStatus);

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
        auto paths = getinventoryPath(inventoryIface);
        std::vector<std::string> ret;
        for (auto p : paths) {
            if (pathIsValidDevice(p)) {
                ret.push_back(p);
            }
        }
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

        auto pair = std::make_pair(std::move(model), std::move(manufacture));
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
        for (auto& it : deviceIds)
        {
            auto& pair = it.second;
            if (pair.first == model && pair.second == manufacture)
            {
                return it.first;
            }
        }
        return "";
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
     * @brief adds inventory association to new version and removes from the old
     *
     * @param versionId
     * @param inventoryPath
     */
    void onUpdateDone(const std::string& versionId,
                      const std::string& inventoryPath) override;

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
    virtual std::string getServiceArgs(const std::string& inventoryPath,
                                       const std::string& imagePath) const = 0;
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
     * @return std::string
     */
    virtual std::string
        getUpdateServiceWithArgs(const std::string& inventoryPath,
                                 const std::string& imagePath) const
    {
        auto args = getServiceArgs(inventoryPath, imagePath);
        auto service = getServiceName();
        auto p = service.find('@');
        assert(p != std::string::npos);
        service.insert(p + 1, args);
        return service;
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
    virtual bool pathIsValidDevice(std::string &p) {
        (void)p;
        return true;
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

    std::map<std::string, std::pair<std::string, std::string>> deviceIds;

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
