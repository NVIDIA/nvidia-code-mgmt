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

#include "activation_listener.hpp"
#include "dbusutils.hpp"
#include "version.hpp"
#include "xyz/openbmc_project/Common/FilePath/server.hpp"
#include "xyz/openbmc_project/Common/UUID/server.hpp"
#include "xyz/openbmc_project/Object/Delete/server.hpp"
#include "xyz/openbmc_project/Software/ExtendedVersion/server.hpp"
#include "xyz/openbmc_project/Software/Version/server.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/timer.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>
#include <xyz/openbmc_project/Software/ExtendedVersion/server.hpp>
#include <xyz/openbmc_project/Software/UpdatePolicy/server.hpp>
#include <xyz/openbmc_project/State/ServiceReady/server.hpp>

#include <functional>
#include <iostream>
#include <queue>
#include <string>

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

using VersionInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Common::server::UUID,
    sdbusplus::xyz::openbmc_project::Software::server::Activation,
    sdbusplus::xyz::openbmc_project::Software::server::ExtendedVersion,
    sdbusplus::xyz::openbmc_project::Common::server::FilePath>;

using DeleteInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Object::server::Delete>;

using UpdatePolicyInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::UpdatePolicy>;

using ServiceReadyInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::ServiceReady>;
using InventoryInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset>;

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
using namespace phosphor::logging;

/**
 * @brief For devices like retimer version interface is populated by gpu manager
 * in case of HGX but for PSU and CPLD it needs to be populated from item
 * updater hence handling it seperately.
 */
using softwareVersionInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Version>;

/**
 * @brief xyz.openbmc_project.Software.Version Interface for dbus
 */

class SoftwareVersion : public softwareVersionInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     */
    SoftwareVersion(sdbusplus::bus::bus& bus, const std::string& objPath) :
        softwareVersionInherit(bus, objPath.c_str(),
                               action::emit_interface_added)

    {
        purpose(sdbusplus::xyz::openbmc_project::Software::server::Version::
                    VersionPurpose::Other);
    }
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

/**@class UpdatePolicy
 *
 *  Concrete implementation of xyz.openbmc_project.Software.UpdatePolicy D-Bus
 *  interface
 *
 */
class UpdatePolicy : public UpdatePolicyInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     */
    UpdatePolicy(sdbusplus::bus::bus& bus, const std::string& objPath) :
        UpdatePolicyInherit(bus, objPath.c_str(), action::emit_interface_added)

    {}
};

/**@class ServiceReady
 *
 *  Concrete implementation of xyz.openbmc_project.State.ServiceReady D-Bus
 *  interface
 *
 */
class ServiceReady : public ServiceReadyInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     */
    ServiceReady(sdbusplus::bus::bus& bus, const std::string& objPath) :
        ServiceReadyInherit(bus, objPath.c_str(), action::emit_interface_added)

    {}
};

/**@class DeviceSKU
 *
 *  Concrete implementation of xyz.openbmc_project.Inventory.Decorator.Asset
 * D-Bus interface
 *
 */
class DeviceSKU : public InventoryInherit
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach t
     *  @param[in] objPath - D-Bus object path
     */
    DeviceSKU(sdbusplus::bus::bus& bus, const std::string& objPath) :
        InventoryInherit(bus, objPath.c_str(), action::emit_interface_added)

    {}
};

/**
 * @brief Version dbus class
 * @author
 * @since Wed Aug 04 2021
 */
class Version : public VersionInherit, public DBUSUtils
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
            const Status& activationStatus, const std::string& model,
            const std::string& manufacturer, eraseFunc callback,
            ActivationListener* activationListener,
            ItemUpdaterUtils* itemUpdaterUtils) :
        VersionInherit(bus, (objPath).c_str(),
                       VersionInherit::action::defer_emit),
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
        uuid(uniqueId);
        path(filePath);
        extendedVersion(versionString);
        activation(activationStatus);

        activationProgress = nullptr;
        updatePolicy = std::make_unique<UpdatePolicy>(bus, objPath);
        updatePolicy->forceUpdate(true);
        deleteObject = std::make_unique<Delete>(bus, objPath, *this);
        timer = nullptr;
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
     * @brief Get the Version Id object
     *
     * @return const std::string&
     */
    const std::string& getVersionId() const
    {
        return versionId;
    }

    /**
     * @brief Timeout handler for non-pldm updates. This method
     *        sets the status to failed if update did not complete
     *        within specified time.
     *
     * @param timeout
     */
    inline void startTimer(uint32_t timeout)
    {
        timer = std::make_unique<sdbusplus::Timer>([this]() {
            if (!deviceQueue.empty())
            {
                log<level::ERR>("Update timed out");
                this->onUpdateFailed();
            }
        });
        timer->start(std::chrono::seconds(timeout), false);
    }

    /**
     * @brief Call back for systemd service fail
     *
     */
    void onUpdateFailed();

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

    /**
     * @brief Create a Log entry for bmcweb to consume
     *
     * @param messageID - redfish message id
     * @param addData - redfish message data
     * @param level - log level
     *
     * @return void
     */
    void createLog(const std::string& messageID,
                   std::map<std::string, std::string>& addData, Level& level);

    /**
     * @brief Log message indicating transfer failed.
     *        This message will be logged when image transfer fails
     *        or item updater times out.
     *
     * @param compName - component name
     * @param compVersion - component version
     *
     * @return void
     */
    void logTransferFailed(const std::string& compName,
                           const std::string& compVersion);

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

    std::unique_ptr<ActivationProgress> activationProgress;

    std::unique_ptr<UpdatePolicy> updatePolicy;

    ActivationListener* activationListener;

    ItemUpdaterUtils* itemUpdaterUtils;

    TargetFilter targetFilter;
    std::unique_ptr<sdbusplus::Timer> timer;
};

} // namespace updater
} // namespace software
} // namespace nvidia
