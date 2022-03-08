
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#ifndef MOCK_UTILS
#include <cpld_util.hpp> // part of nvidia-cpld
namespace cpldcommonutils = nvidia::cpld::common;
#else
#include <mock_util.hpp> // mock
namespace cpldcommonutils = nvidia::mock::common;
#endif
namespace nvidia
{
namespace software
{
namespace updater
{
class CPLDDevice : public cpldcommonutils::Util
{
    std::string name, inventoryPath;

  public:
    CPLDDevice(const std::string& objPath, uint8_t busN, uint8_t address,
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
};
/**
 * @brief CPLD concrete class for Item updater 
 * @author
 * @since Wed Sept 13 2021
 */
class CPLDItemUpdater : public BaseItemUpdater
{
    std::vector<std::unique_ptr<CPLDDevice>> invs;

  public:
    /**
     * @brief Construct a new CPLDItemUpdater object
     * 
     * @param bus 
     */
    CPLDItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, CPLD_SUPPORTED_MODEL, CPLD_INVENTORY_IFACE,
                            "CPLD", CPLD_BUSNAME_UPDATER, CPLD_UPDATE_SERVICE,
                            false, CPLD_BUSNAME_INVENTORY)
    {
        nlohmann::json fruJson = cpldcommonutils::loadJSONFile(
            "/usr/share/nvidia-power-manager/cpld_config.json");
        if (fruJson == nullptr)
        {
            log<level::ERR>("InternalFailure when parsing the JSON file");
            return;
        }
        for (const auto& fru : fruJson.at("CPLD"))
        {
            try
            {
                const auto baseinvInvPath =
                    "/xyz/openbmc_project/inventory/system/board/Cpld";
                std::string id = fru.at("Index");
                std::string busN = fru.at("Bus");
                std::string address = fru.at("Address");
                std::string invpath = baseinvInvPath + id;

                uint8_t busId = std::stoi(busN);
                uint8_t devAddr = std::stoi(address, nullptr, 16);

                auto invMatch = std::find_if(
                    invs.begin(), invs.end(), [&invpath](auto& inv) {
                        return inv->getInventoryPath() == invpath;
                    });
                if (invMatch != invs.end())
                {
                    continue;
                }
                auto inv =
                    std::make_unique<CPLDDevice>(invpath, busId, devAddr, id);
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
     * @return std::string 
     */
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath) const override
    {

        // The systemd unit shall be escaped
        (void)inventoryPath;
        std::string args = "";
        args += "\\x20\\x2df"; // -f
        args += "\\x20";
        args += imagePath;
        args += "\\x20\\x2dt"; // -t
        args += "\\x20";
        args += "1";
        args += "\\x20\\x2db"; //-b
        args += "\\x20";
        args += "10";
        args += "\\x20\\x2dd"; //-d
        args += "\\x20";
        args += "0x55";
        std::replace(args.begin(), args.end(), '/', '-');

        return args;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
