
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

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
                        "PCIeRetimerFirmware", RT_BUSNAME_UPDATER,
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
    std::string
        getManufacturer(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @return std::string
     */
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath) const override
    {

        std::string args = "";
        if (updateAllTogether())
        {
            args += "\\x20";
            args += std::to_string(invs[0]->getBus()); // pull first device bus
            args += "\\x20";
            args += std::to_string(invs.size()); // n devices
            args += "\\x20";
            args += imagePath; // image
            args += "\\x20";
            args += "0"; // path
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
};

} // namespace updater
} // namespace software
} // namespace nvidia
