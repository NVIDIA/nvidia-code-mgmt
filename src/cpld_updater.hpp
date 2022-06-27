
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
    uint8_t i2cBus;
    uint32_t imageSelect;

  public:
    CPLDDevice(const std::string& objPath, uint8_t busN, uint8_t address,
               uint32_t imageNo, const std::string& name) :
        name(name),
        inventoryPath(objPath), i2cBus(busN), imageSelect(imageNo)
    {
        b = busN;
        d = address;
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

    const std::string getBusNum()
    {
        std::ostringstream convert;
        convert << (int)i2cBus;
        std::string ret = convert.str();
        return ret;
    }

    const std::string getImageSelect()
    {
        std::ostringstream convert;
        convert << (int)imageSelect;
        std::string ret = convert.str();
        return ret;
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
        BaseItemUpdater(bus, CPLD_SUPPORTED_MODEL, CPLD_INVENTORY_IFACE, "CPLD",
                        CPLD_BUSNAME_UPDATER, CPLD_UPDATE_SERVICE, false,
                        CPLD_BUSNAME_INVENTORY)
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
                uint32_t imageselect = fru.at("ImageSelect");
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
                auto inv = std::make_unique<CPLDDevice>(invpath, busId, devAddr,
                                                        imageselect, id);
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
    virtual std::string
        getServiceArgs(const std::string& inventoryPath,
                       const std::string& imagePath,
                       const std::string& version,
                       const TargetFilter /* &targetFilter */) const override
    {

        // The systemd unit shall be escaped
        std::string args = "";
        for (auto& inv : invs)
        {
            if (inv->getInventoryPath() == inventoryPath)
            {
                args += "\\x20";
                args += inv->getBusNum(); // <Bus-Number>
                args += "\\x20";
                args +=
                    inv->getImageSelect(); /* <Image-Select> As per Nvidia
                          Doc, Image-select = 2 : will flash image in
                              CFM0(0xfbcfffff) backup,
                          Image-select = 3 : will
                              flash image in CFM1(0xfcbfffff) and
                              CFM2(0xfcafffff) By default it was select as 3*/
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
