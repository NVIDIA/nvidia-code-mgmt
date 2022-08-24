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
    std::string model;
    std::string manufacturer;
    uint32_t cpldN;

  public:
    CPLDDevice(const std::string& objPath, uint8_t busN, uint8_t address,
               uint32_t imageNo, const std::string& name,
               const std::string& model, const std::string& manufacturer,
               uint32_t cpldN) :
        name(name),
        inventoryPath(objPath), i2cBus(busN), imageSelect(imageNo),
        model(model), manufacturer(manufacturer), cpldN(cpldN)
    {
        b = busN;
        d = address;
    }

    /*
     *@brief Gets index value
     *
     * Name is related to index value from cpld_config.json file.
     * As from i2c tool we are not able to get version value, we are using index
     * value for version , inorder to create separate object path for different
     * Devices.
     *
     * return string: index number
     *
     */
    std::string getVersion() const override
    {
        return name;
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

    /*
     *@brief Gets model number of device
     *
     *return string: model number
     *
     */
    std::string getModel() const override
    {
        return model;
    }

    /*
     *@brief gets Manufacture number of device
     *
     *return string: Manufacture number
     *
     */
    std::string getManufacturer() const override
    {
        return manufacturer;
    }

    /*
     *@brief CPLD Device Selection
     *
     *return string: CPLD number
     *
     */
    const std::string getCPLDDeviceNum()
    {
        std::ostringstream convert;
        convert << (int)cpldN;
        std::string ret = convert.str();
        return ret;
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
                std::string model = fru.at("Model");
                std::string manufacturer = fru.at("Manufacturer");
                uint32_t cpldDeviceN = fru.at("CPLDDeviceNo");
                std::string invpath = baseinvInvPath + id;
                for (auto& it : deviceIds)
                {
                    auto& pair = it.second;
                    if (get<0>(pair) == model && get<1>(pair) == manufacturer)
                    {
                        get<2>(pair) = id;
                        break;
                    }
                }
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
                auto inv = std::make_unique<CPLDDevice>(
                    invpath, busId, devAddr, imageselect, id, model,
                    manufacturer, cpldDeviceN);
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
        const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
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
                          Doc, Image-select = 2/0 (MID/MB): will flash image in
                              CFM0(0xfbcfffff) backup,
                          Image-select = 3/1 (MID/MB): will
                              flash image in CFM1(0xfcbfffff) and
                              CFM2(0xfcafffff) By default it was select as 3*/
                args += "\\x20";
                args += imagePath;
                args += "\\x20";
                args += inv->getCPLDDeviceNum();
                args += "\\x20";
                args += version; // for Message Registry
                args += "\\x20";
                break;
            }
        }

        std::replace(args.begin(), args.end(), '/', '-');

        return args;
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

    std::string getIdProperty(const std::string& identifier) override
    {
            std::string deviceVersion;
            for (auto& it : deviceIds)
            {
                auto& pair = it.second;
                if (it.first == identifier)
                {
                    deviceVersion = get<2>(pair);
                    break;
                }
                if (get<2>(pair) == identifier)
                {
                    deviceVersion = identifier;
                    break;
                }
            }

            if (deviceVersion.empty())
                return "";
            return createVersionID(getName(), deviceVersion);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
