#pragma once

#include "activation_listener.hpp"
#include "association_interface.hpp"
#include "dbusutils.hpp"
#include "types.hpp"
#include "version.hpp"
#include "xyz/openbmc_project/Common/FilePath/server.hpp"
#include "xyz/openbmc_project/Common/UUID/server.hpp"
#include "xyz/openbmc_project/Object/Delete/server.hpp"
#include "xyz/openbmc_project/Software/ExtendedVersion/server.hpp"
#include "xyz/openbmc_project/Software/Version/server.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <functional>
#include <iostream>
#include <queue>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <string>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Association/server.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationBlocksTransition/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>
#include <xyz/openbmc_project/Software/ExtendedVersion/server.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{

namespace MatchRules = sdbusplus::bus::match::rules;

namespace sdbusRule = sdbusplus::bus::match::rules;

typedef std::function<void(std::string)> eraseFunc;

using PropertyType = std::variant<std::string, bool>;

using Properties = std::map<std::string, PropertyType>;

using ActivationProgressInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::ActivationProgress>;

using ActivationBlocksTransitionInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::
        ActivationBlocksTransition>;

using VersionInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions,
    sdbusplus::xyz::openbmc_project::Common::server::UUID,
    sdbusplus::xyz::openbmc_project::Software::server::Activation,
    sdbusplus::xyz::openbmc_project::Software::server::Version,
    sdbusplus::xyz::openbmc_project::Software::server::ExtendedVersion,
    sdbusplus::xyz::openbmc_project::server::Association,
    sdbusplus::xyz::openbmc_project::Common::server::FilePath>;

using DeleteInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Object::server::Delete>;

/**
 * @brief activation class for dbus
 * @author
 * @since Wed Aug 04 2021
 */
class ActivationBlocksTransition : public ActivationBlocksTransitionInherit
{
  public:
    /**
     * @brief Construct a new Activation Blocks Transition object
     *
     * @param bus
     * @param path
     */
    ActivationBlocksTransition(sdbusplus::bus::bus& bus,
                               const std::string& path) :
        ActivationBlocksTransitionInherit(bus, path.c_str(),
                                          action::emit_interface_added),
        bus(bus)
    {
        enableRebootGuard();
    }

    /**
     * @brief Destroy the Activation Blocks Transition object
     *
     */
    ~ActivationBlocksTransition()
    {
        disableRebootGuard();
    }

  private:
    sdbusplus::bus::bus& bus;

    /**
     * @brief Enable rebootguard
     *
     */
    void enableRebootGuard();

    /**
     * @brief disable reboot guard
     *
     */
    void disableRebootGuard();
};

/**
 * @brief ActivationProgress for dbus
 * @author
 * @since Wed Aug 04 2021
 */
class ActivationProgress : public ActivationProgressInherit
{
  public:
    /**
     * @brief Construct a new Activation Progress object
     *
     * @param bus
     * @param path
     */
    ActivationProgress(sdbusplus::bus::bus& bus, const std::string& path) :
        ActivationProgressInherit(bus, path.c_str(),
                                  action::emit_interface_added)
    {
        progress(0);
    }
};

class Version;
class Delete;

/**
 * @brief Delete dbus object
 * @author
 * @since Wed Aug 04 2021
 */
class Delete : public DeleteInherit
{
  public:
    /**
     * @brief Construct a new Delete object
     *
     * @param bus
     * @param path
     * @param parent
     */
    Delete(sdbusplus::bus::bus& bus, const std::string& path, Version& parent) :
        DeleteInherit(bus, path.c_str(), action::emit_interface_added),
        parent(parent)
    {
        // Empty
    }

    /**
     * @brief delete
     *
     */
    void delete_() override;

  private:
    Version& parent;
};

/**
 * @brief Version dbus class
 * @author
 * @since Wed Aug 04 2021
 */
class Version : public VersionInherit,
                public DBUSUtils,
                public AssociationInterface
{
  public:
    using Status = Activations;
    /**
     * @brief Construct a new Version object
     *
     * @param bus
     * @param versionString
     * @param objPath
     * @param uniqueId
     * @param versionId
     * @param filePath
     * @param assoc
     * @param activationStatus
     * @param model
     * @param manufacturer
     * @param callback
     * @param activationListener
     * @param itemUpdaterUtils
     */
    Version(sdbusplus::bus::bus& bus, const std::string& versionString,
            const std::string& objPath, const std::string& uniqueId,
            const std::string& versionId, const std::string& filePath,
            const AssociationList& assoc, const Status& activationStatus,
            const std::string& model, const std::string& manufacturer,
            eraseFunc callback, ActivationListener* activationListener,
            ItemUpdaterUtils* itemUpdaterUtils) :
        VersionInherit(bus, (objPath).c_str(), true),
        DBUSUtils(bus), eraseCallback(callback), versionId(versionId),
        objPath(objPath), model(model), manufacturer(manufacturer),
        verstionStr(versionString),
        systemdSignals(
            bus,
            sdbusRule::type::signal() + sdbusRule::member("JobRemoved") +
                sdbusRule::path("/org/freedesktop/systemd1") +
                sdbusRule::interface("org.freedesktop.systemd1.Manager"),
            std::bind(&Version::unitStateChange, this, std::placeholders::_1)),
        activationListener(activationListener),
        itemUpdaterUtils(itemUpdaterUtils)
    {
        // Set properties.
        version(versionString);
        uuid(uniqueId);
        path(filePath);
        extendedVersion(filePath);
        associations(assoc);
        activation(activationStatus);
        purpose(sdbusplus::xyz::openbmc_project::Software::server::Version::
                    VersionPurpose::Other);

        activationBlocksTransition = nullptr;
        activationProgress = nullptr;
        deleteObject = std::make_unique<Delete>(bus, objPath, *this);
        // Emit deferred signal.
        emit_object_added();
    }

    /**
     * @brief Get the Manufacturer object
     *
     * @return std::string
     */
    std::string getManufacturer()
    {
        return manufacturer;
    }

    /**
     * @brief Get the Model object
     *
     * @return std::string
     */
    std::string getModel()
    {
        return model;
    }

    /**
     * @brief Get the Version String object
     *
     * @return const std::string&
     */
    const std::string& getVersionString() const
    {
        return verstionStr;
    }

    /**
     * @brief Create a Active Association object
     *
     * @param path
     */
    void createActiveAssociation(const std::string& path)
    {
        auto assocs = associations();

        assocs.emplace_back(std::make_tuple(ACTIVE_FWD_ASSOCIATION,
                                            std::string(""), path));
        assocs.emplace_back(std::make_tuple(ACTIVE_REV_ASSOCIATION,
                                            ACTIVE_FWD_ASSOCIATION, std::string(SOFTWARE_OBJPATH)));
        associations(assocs);
    }

    /**
     * @brief add function association to version
     *
     * @param path
     */
    void addFunctionalAssociation(const std::string& path)
    {
        auto assocs = associations();
        assocs.emplace_back(std::make_tuple(FUNCTIONAL_FWD_ASSOCIATION,
                                            std::string(""), path));
        assocs.emplace_back(std::make_tuple(FUNCTIONAL_REV_ASSOCIATION,
                                            FUNCTIONAL_FWD_ASSOCIATION, std::string(SOFTWARE_OBJPATH)));
        associations(assocs);
    }

    /**
     * @brief Add updatable association to version
     *
     * @param path
     */
    void addUpdateableAssociation(const std::string& path)
    {
        auto assocs = associations();
        assocs.emplace_back(std::make_tuple(UPDATEABLE_FWD_ASSOCIATION,
                                            std::string(""), path));
        assocs.emplace_back(std::make_tuple(UPDATEABLE_REV_ASSOCIATION,
                                            UPDATEABLE_FWD_ASSOCIATION, std::string(SOFTWARE_OBJPATH)));
        associations(assocs);
    }

    /** @brief Activation */
    using VersionInherit::activation;

    /**
     * @brief Get the Object Path object
     *
     * @return const std::string&
     */
    const std::string& getObjectPath() const
    {
        return objPath;
    }

    /**
     * @brief requestedActivation to initate the update
     *
     * @param value
     * @return RequestedActivations
     */
    RequestedActivations
        requestedActivation(RequestedActivations value) override;

    /**
     * @brief Initates the activation
     *
     * @param value
     * @return Status
     */
    Status activation(Status value) override;

    /**
     * @brief removes association from version
     *
     * @param path
     */
    void removeAssociation(const std::string& path) override;

    /**
     * @brief Get the Version Id object
     *
     * @return const std::string&
     */
    const std::string& getVersionId() const
    {
        return versionId;
    }

  public:
    std::unique_ptr<Delete> deleteObject;

    eraseFunc eraseCallback;

  private:
    /**
     * @brief unit state change callback
     *
     * @param msg
     */
    void unitStateChange(sdbusplus::message::message& msg);

    /**
     * @brief calls update systemd service
     *
     * @param inventoryPath
     * @return true
     * @return false
     */
    bool doUpdate(const std::string& inventoryPath);

    /**
     * @brief calls update services for all inventory paths
     *
     * @return true
     * @return false
     */
    bool doUpdate();

    /**
     * @brief Call back for systemd service
     *
     */
    void onUpdateDone();

    /**
     * @brief Call back for systemd service fail
     *
     */
    void onUpdateFailed();

    /**
     * @brief Prepares for image update
     *
     * @return Status
     */
    Status startActivation();

    /**
     * @brief Updates Version status and closes update state machine
     * @return (void)
     */
    void finishActivation();

    /**
     * @brief Checks image compatibility
     * 
     * @param inventoryPath 
     * @return true 
     * @return false 
     */
    bool isCompatible(const std::string& inventoryPath);

    /**
     * @brief Moves images to persistant path
     * 
     */
    void storeImage();

    /**
     * @brief Get the Update Service object
     * 
     * @param inventoryPath 
     * @return std::string 
     */
    std::string getUpdateService(const std::string& inventoryPath);

  private:
    std::string versionId;

    std::string objPath;

    std::string model;

    std::string manufacturer;

    std::string verstionStr;

    sdbusplus::bus::match_t systemdSignals;

    std::queue<std::string> deviceQueue;

    uint32_t progressStep;

    std::string deviceUpdateUnit;

    std::string currentUpdatingDevice;

    std::unique_ptr<ActivationBlocksTransition> activationBlocksTransition;

    std::unique_ptr<ActivationProgress> activationProgress;

    ActivationListener* activationListener;

    ItemUpdaterUtils* itemUpdaterUtils;
};

} // namespace updater
} // namespace software
} // namespace nvidia
