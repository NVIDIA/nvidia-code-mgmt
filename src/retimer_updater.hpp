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
#include "version.hpp"
#include <bitset>
#include <filesystem>
#include "fmt/core.h"

#ifndef MOCK_UTILS
#include <rt_util.hpp> // part of nvidia-retimer
namespace rtcommonutils = nvidia::retimer::common;
#else
#include <mock_util.hpp> // mock
namespace rtcommonutils = nvidia::mock::common;
#endif

namespace nvidia
{
namespace software
{
namespace updater
{

class RTDevice : public rtcommonutils::Util
{
    std::string name, inventoryPath;

  public:
    RTDevice(const std::string& objPath, int busN, int address,
             const std::string& name) :
        name(name),
        inventoryPath(objPath)
    {
        b = busN;
        d = address;
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

    int getBus() const
    {
        return b;
    }

    int getAddress() const
    {
        return d;
    }

    const std::string& getId()
    {
        return name;
    }
};

/**
 * @brief
 * @author
 * @since Wed Aug 04 2021
 */
class ReTimerItemUpdater : public BaseItemUpdater
{
    std::vector<std::unique_ptr<RTDevice>> invs;
    std::unique_ptr<ServiceReady> serviceReadyObj;
    std::vector<sdbusplus::bus::match_t> updateMatchRules;
    std::unique_ptr<DeviceSKU> deviceSKUInventoryObj;
    const std::string objPath = std::string(SOFTWARE_OBJPATH) + "/" + std::string(RT_NAME);

  public:
    /**
     * @brief Construct a new Re Timer Item Updater object
     *
     * @param bus dbus reference
     * @param together update everything together
     */
    ReTimerItemUpdater(sdbusplus::bus::bus& bus, bool together) :
        BaseItemUpdater(bus, RT_SUPPORTED_MODEL, RT_INVENTORY_IFACE,
                        RT_NAME, RT_BUSNAME_UPDATER,
                        RT_UPDATE_SERVICE, together, RT_BUSNAME_INVENTORY)
    {

        nlohmann::json fruJson = rtcommonutils::loadJSONFile(
            "/usr/share/nvidia-retimer/rt_config.json");
        if (fruJson == nullptr)
        {
            log<level::ERR>("InternalFailure when parsing the JSON file");
            return;
        }
        for (const auto& fru : fruJson.at("RT"))
        {
            try
            {
                const auto baseinvInvPath = RT_INVENTORY_PATH;
                std::string id = fru.at("Index");
                std::string busN = fru.at("Bus");
                std::string address = fru.at("Address");
                std::string invpath = baseinvInvPath + id;

                int busId = std::stoi(busN);
                int devAddr = std::stoi(address, nullptr, 16);

                auto invMatch = std::find_if(
                    invs.begin(), invs.end(), [&invpath](auto& inv) {
                        return inv->getInventoryPath() == invpath;
                    });
                if (invMatch != invs.end())
                {
                    continue;
                }
                auto inv =
                    std::make_unique<RTDevice>(invpath, busId, devAddr, id);
                invs.emplace_back(std::move(inv));
            }
            catch (const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
        }
        serviceReadyObj = std::make_unique<ServiceReady>(bus, objPath);
        serviceReadyObj->state(sdbusplus::xyz::openbmc_project::State
                ::server::ServiceReady::States::Disabled);
        createSKUInventory(bus, objPath);
    }

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(const std::string& inventoryPath) const override;

    /**
     * @brief Get retimer SKU from inventory object
     *      Assumes that all retimers on the platform are from the same 
     *      manufacturer with the same deviceID
     *
     * @return std::string
     */
    std::string getSKU() const;

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
     * @brief Get retimer the devices to update object based on target filters
     * 
     * @param targetFilter 
     * @return std::bitset representing which retimers to update. At any bit
     *                     1 represents that the retimer is to be updated
     *                     and 0 for skipping update to that retimer
     */
    std::bitset<SUPPORTED_RETIMERS> applyTargetFilter(const TargetFilter& targetFilter) const
    {
        std::bitset<SUPPORTED_RETIMERS> devices;
        if (targetFilter.type == TargetFilterType::UpdateAll)
        {
            devices.set();
        }
        else if(targetFilter.type == TargetFilterType::UpdateSelected)
        {
            for(auto& target : targetFilter.targets)
            {
                uint deviceId;
                int ret = std::sscanf(target.c_str(), RT_SW_ID_FORMAT, &deviceId);
                if (ret > 0 && deviceId < SUPPORTED_RETIMERS)
                {
                    devices[deviceId] = 1;
                }
            }
        }
        //else targetFilter type is UpdateNone, all bits are set to 0 by default
        return devices;
    }

    /**
     * @brief Get retimer the devices to update object based on target firmware 
     * version and current version
     * 
     * @param version 
     * @param devices std::bitset representing which devices are currently 
     *                not filtered out through target filtering
     * @return std::bitset representing which retimers to update. At any bit
     *                     1 represents that the retimer is to be updated
     *                     and 0 for skipping update to that retimer
     */
    std::bitset<SUPPORTED_RETIMERS> filterDevicesBasedOnVersionCheck(const std::string& pkgVersion, std::bitset<SUPPORTED_RETIMERS>& devices) const
    {
        for (const auto& inv: invs)
        {
            const auto currTarget = std::filesystem::path(inv->getInventoryPath()).filename().string();
            const auto currentVersion = getVersion(inv->getInventoryPath());
            if (currentVersion.empty())
            {
                log<level::WARNING>(fmt::format("Unable to fetch the version from Inventory for {}", currTarget).c_str());
                continue;
            }
            uint deviceId;
            int ret = std::sscanf(std::filesystem::path(inv->getInventoryPath()).filename().string().c_str(), RT_INVENTORY_FORMAT, &deviceId);
            if (ret < 0 || deviceId > SUPPORTED_RETIMERS)
            {
                continue;
            }
            if (devices[deviceId] != 1)
            {
                continue;
            }
            if (!currentVersion.empty() and pkgVersion.compare(currentVersion) == 0)
            {
                log<level::INFO>(fmt::format("Image Version is identical for {}, skipping update. "
                        "Image version: {} Retimer Version: {}", currTarget, pkgVersion, currentVersion).c_str());
                logIdenticalImageInfo(currTarget);
                devices[deviceId] = 0;
            }
        }
        return devices;
    }

    /**
     * @brief Get retimer the devices to update object based on target filters,
     * version check and force update
     * 
     * @param targetFilter 
     * @param version 
     * @return std::string representing which retimers to update. At any index
     *                     1 represents that the retimer is to be updated
     *                     and 0 for skipping update to that retimer
     */
    std::string getDevicesToUpdate(const TargetFilter& targetFilter, const std::string& version, bool forceUpdate) const
    {
        std::bitset<SUPPORTED_RETIMERS> devices = applyTargetFilter(targetFilter);
        if (!forceUpdate) 
        {
            filterDevicesBasedOnVersionCheck(version, devices);
        }
        return std::to_string(devices.to_ulong());
    }

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @param forceUpdate
     * @return std::string
     */
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath,
                       const std::string& version,
                       const TargetFilter &targetFilter,
                       const bool forceUpdate) const override
    {

        std::string args = "";
        if (updateAllTogether())
        {
            std::string devicesBits = getDevicesToUpdate(targetFilter, version, forceUpdate);
            args += "\\x20";
            args += std::to_string(invs[0]->getBus()); // pull first device bus
            args += "\\x20";
            args += devicesBits; // devices to update
            args += "\\x20";
            args += imagePath; // image
            args += "\\x20";
            args += "0"; // path
            args += "\\x20";
            args += version; // version string for message registry
        }
        else
        {
            for (auto& inv : invs)
            {
                if (inv->getInventoryPath() == inventoryPath)
                {
                    args += "\\x20";
                    args += std::to_string(inv->getBus());
                    args += "\\x20";
                    args += std::to_string(inv->getAddress());
                    args += "\\x20";
                    args += imagePath;
                    break;
                }
            }
        }
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }

    std::string getServiceName() const
    {
        if (updateAllTogether())
        {
            return BaseItemUpdater::getServiceName();
        }
        else
        {
            return RT_UPDATE_SINGLE_SERVICE;
        }
    }

    bool pathIsValidDevice(std::string& p)
    {
        for (auto& inv : invs)
        {
            if (inv->getInventoryPath() == p)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief This method overrides getItemUpdaterInventoryPaths.
     *        Gets the inventory from RTDevice inventory and does not
     *        rely on objects created by GpuMgr.
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        for (auto& inv : invs)
        {
            ret.emplace_back(inv->getInventoryPath());
        }
        return ret;
    }

    /**
     * @brief This method overrides GetServiceName and returns service name from
     * config instead of looking at mapper
     *
     * @param path - object path
     * @param interface - dbus interface
     * @return std::string
     */
    std::string getDbusService(const std::string& /* path */,
                    const std::string& /* interface */) override
    {
        return RT_BUSNAME_INVENTORY;
    }

    std::string validateTarget(const sdbusplus::message::object_path& target)
    {
        uint deviceId;
        int ret = std::sscanf(target.filename().c_str(), RT_SW_ID_FORMAT, &deviceId);
        if (ret > 0 && deviceId < SUPPORTED_RETIMERS)
        {
            std::string invPath = RT_INVENTORY_PATH + std::to_string(deviceId);
            if(getService(invPath.c_str(), ASSET_IFACE) != "")
            {
                return target.filename();
            }
        }
        return "";
    }
    /**
     * @brief Get timeout from config file for retimer
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return RT_UPDATE_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for retimer inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief callback method for creating retimer update service when CSM's state
     * changes to enabled
     *
     * @param msg - sdbusplus message 
     * @param version - Version object corresponding to the ItemUpdater
     * @param deviceUpdateUnit - systemd update service 
     *
     * @return 
     */
    void createUpdateServiceMsg(sdbusplus::message::message& msg,
            Version* version, const std::string& deviceUpdateUnit)
    {
        std::string interface{};
        Properties properties{};
        std::string state{};
        msg.read(interface, properties);

        auto p = properties.find(STATE);
        if (p == properties.end())
        {
            return;
        }

        state = std::get<std::string>(p->second);

        if (state == "xyz.openbmc_project.State.FeatureReady.States.Enabled")
        {
            createUpdateService(version, deviceUpdateUnit);
        }
        startWatchingActivation();
    }

    /**
     * @brief creates retimer updater systemd service
     *
     * @param version - Version object corresponding to the ItemUpdater
     * @param deviceUpdateUnit - systemd update service 
     *
     * @return 
     */
    void createUpdateService(Version* version, const std::string& deviceUpdateUnit)
    {
        try
        {
            auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                              SYSTEMD_INTERFACE, "StartUnit");
            method.append(deviceUpdateUnit, "replace");
            bus.call_noreply(method);
            version->startTimer(getTimeout());
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            log<level::ERR>("Error starting service", entry("ERROR=%s", e.what()),
                    entry("SERVICE=%s", deviceUpdateUnit.c_str()));
            version->onUpdateFailed();
        }
    }

    /**
     * @brief callback method to modify ServiceReady properties based on Activation
     * status of the ItemUpdater object
     *
     * @param msg - sdbusplus message 
     *
     * @return 
     */
    void onActivationChanged(sdbusplus::message::message& msg)
    {
        std::string interface;
        Properties properties;
        std::string activationState;
        msg.read(interface, properties);

        auto p = properties.find(ACTIVATION);
        if (p == properties.end())
        {
            return;
        }

        activationState = std::get<std::string>(p->second);
        if (activationState == "xyz.openbmc_project.Software.Activation.Activations.Failed" or 
                activationState == "xyz.openbmc_project.Software.Activation.Activations.Active")
        {
            serviceReadyObj->state(sdbusplus::xyz::openbmc_project::State
                    ::server::ServiceReady::States::Disabled);
            updateMatchRules.clear();
        }

    }

    /**
     * @brief Triggers retimer update mechanism.
     *
     * @param version - Version object corresponding to the ItemUpdater
     * @param deviceUpdateUnit - systemd update service 
     *
     * @return true if triggering the update is sucessful
     */
    bool doUpdate(Version* version,
            const std::string& deviceUpdateUnit) override
    {
        serviceReadyObj->state(sdbusplus::xyz::openbmc_project::State
                ::server::ServiceReady::States::Enabled);
        startWatchingCSM(version, deviceUpdateUnit);
        return true;
    }
    
    /**
     * @brief subscribes to the ItemUpdater D-Bus object's Activation interface
     * to monitor success/failure of the update. Changes ServiceReady property on 
     * update completion
     *
     * @return
     */
    void startWatchingActivation()
    {
        // Subscribe to the Item Updater's Activation changes
        updateMatchRules.emplace_back(
            bus, MatchRules::propertiesChanged(objPath.c_str(), ACTIVATE_INTERFACE),
            std::bind(&ReTimerItemUpdater::onActivationChanged, this,
                      std::placeholders::_1)); // For present
    }

    /**
     * @brief subscribes to CSM's Feature Ready interface when update triggered
     *
     * @param deviceUpdateUnit - systemd update service 
     *
     * @return
     */
    void startWatchingCSM(Version* version, const std::string& deviceUpdateUnit)
    {
        // Subscribe to the CSM's ServiceReady interface
        updateMatchRules.emplace_back(
            bus, MatchRules::propertiesChanged(CSM_OBJ_PATH,
                "xyz.openbmc_project.State.FeatureReady"),
            std::bind(&ReTimerItemUpdater::createUpdateServiceMsg, this,
                      std::placeholders::_1, version, deviceUpdateUnit)); // For present
    }

    /**
     * @brief callback method to update sku on the RT.Updater object
     *
     * @param msg - unused
     * @return void
     */
    void onSWInventoryChangedMsg([[maybe_unused]] sdbusplus::message::message& msg)
    {
        updateSKU();
    }

    /**
     * @brief sets the sku property on the inventory interface of the RT.Updater object
     *
     * @return void
     */
    void updateSKU()
    {
        deviceSKUInventoryObj->sku(getSKU());
    }

    /**
     * @brief creates SKU inventory interface on the RT.Updater object
     * and starts watching the inventory object
     *
     * @param bus - bus on which the object is present
     * @param objPath - dbus path of the object
     * @return void
     */
    void createSKUInventory(sdbusplus::bus::bus& bus,
                                const std::string& objPath)
    {

        deviceSKUInventoryObj = std::make_unique<DeviceSKU>(bus, objPath);
        updateSKU();

        std::string inventoryObjPath = std::string(RT_INVENTORY_PATH) + invs.at(0)->getId();
        startWatchingInventory(inventoryObjPath);
    }

    /**
     * @brief creates match rules to update sku on interfaces added and 
     * properties changed signals
     *
     * @param inventoryObjPath - dbus path of the inventory object to watch
     * @return void
     */
    void startWatchingInventory(const std::string& inventoryObjPath)
    {
        // Subscribe to the Inventory Object's PropertiesChanged signal
        deviceMatches.emplace_back(
            bus, MatchRules::propertiesChanged(inventoryObjPath.c_str(), ASSET_IFACE),
            std::bind(&ReTimerItemUpdater::onSWInventoryChangedMsg, this,
                      std::placeholders::_1)); // For present
        
        // Subscribe to the Inventory Object's InterfacesAdded signal
        // for when the object is created
        deviceMatches.emplace_back(
            bus, MatchRules::interfacesAdded(inventoryObjPath.c_str()),
            std::bind(&ReTimerItemUpdater::onSWInventoryChangedMsg, this,
                      std::placeholders::_1)); // For present
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
