
#pragma once
#include "config.h"

#include "base_item_updater.hpp"
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
    }

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(const std::string& inventoryPath) const override;

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
};

} // namespace updater
} // namespace software
} // namespace nvidia
