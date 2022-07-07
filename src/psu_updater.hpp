
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <sstream>

#ifndef MOCK_UTILS
#include <psu_util.hpp> // part of nvidia-power-supply
namespace psucommonutils = nvidia::power::common;
#else
#include <mock_util.hpp> // mock
namespace psucommonutils = nvidia::mock::common;
#endif
namespace nvidia
{
namespace software
{
namespace updater
{
class PowerSupplyDevice : public psucommonutils::PSShellIntf
{
    std::string name, inventoryPath;
    uint32_t I2cBus;
    uint32_t I2cSlaveAddress;
    uint8_t index;

  public:
    PowerSupplyDevice(const std::string& objPath, const std::string& name,
                      std::string id, uint32_t bus, uint32_t slaveAddress) :
        psucommonutils::PSShellIntf(id, "psui2ccmd.sh"),
        name(name), inventoryPath(objPath), I2cBus(bus),
        I2cSlaveAddress(slaveAddress)
    {
        index = std::stoi(id);
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

    const std::string& getName() const
    {
        return name;
    }
    const std::string getIndex()
    {
        std::ostringstream convert;
        convert << (int)index;
        std::string ret = convert.str();
        return ret;
    }
    const std::string getBusNum()
    {
        std::ostringstream convert;
        convert << (int)I2cBus;
        std::string ret = convert.str();
        return ret;
    }
    const std::string getSlaveAddress()
    {
        std::ostringstream convert;
        convert << (int)I2cSlaveAddress;
        std::string ret = convert.str();
        return ret;
    }
};
/**
 * @brief PSU item updater
 * @author
 * @since Wed Aug 04 2021
 */
class PSUItemUpdater : public BaseItemUpdater
{
    std::vector<std::unique_ptr<PowerSupplyDevice>> invs;

  public:
    /**
     * @brief Construct a new PSUItemUpdater object
     *
     * @param bus
     */
    PSUItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, PSU_SUPPORTED_MODEL, PSU_INVENTORY_IFACE, "PSU",
                        PSU_BUSNAME_UPDATER, PSU_UPDATE_SERVICE, false,
                        PSU_BUSNAME_INVENTORY)
    {
        nlohmann::json fruJson = psucommonutils::loadJSONFile(
            "/usr/share/nvidia-power-manager/psu_config.json");
        if (fruJson == nullptr)
        {
            log<level::ERR>("InternalFailure when parsing the JSON file");
            return;
        }
        for (const auto& fru : fruJson.at("PowerSupplies"))
        {
            try
            {

                const auto baseinvInvPath =
                    "/xyz/openbmc_project/inventory/system/"
                    "chassis/motherboard/powersupply";
                std::string id = fru.at("Index");
                std::string invpath = baseinvInvPath + id;
                uint32_t busnum = fru.at("I2cBus");
                uint32_t slaveaddress = fru.at("I2cSlaveAddress");

                auto invMatch = std::find_if(
                    invs.begin(), invs.end(), [&invpath](auto& inv) {
                        return inv->getInventoryPath() == invpath;
                    });
                if (invMatch != invs.end())
                {
                    continue;
                }
                auto inv = std::make_unique<PowerSupplyDevice>(
                    invpath, "powersupply" + id, id, busnum, slaveaddress);
                invs.emplace_back(std::move(inv));
            }
            catch (const std::exception& e)
            {
                std::cerr << e.what() << std::endl;
            }
        }
    }
    // TODO add VDT methods here
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
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getServiceArgs(
        const std::string& inventoryPath, const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {

        // The systemd unit shall be escaped
        std::string args = "";
        for (auto& inv : invs)
        {
            if (inv->getInventoryPath() == inventoryPath)
            {
                args += "\\x20";
                args += inv->getBusNum();
                args += "\\x20";
                args += inv->getSlaveAddress();
                args += "\\x20";
                args += imagePath;
                args += "\\x20";
                break;
            }
        }
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
