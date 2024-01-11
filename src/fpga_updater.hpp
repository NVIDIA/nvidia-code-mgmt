
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#ifndef MOCK_UTILS
#include <fpga_util.hpp> // part of nvidia-cec
namespace fpga_ceccommonutils = nvidia::cec::common;
#else
#include <mock_util.hpp> // mock
namespace fpga_ceccommonutils = nvidia::mock::common;
#endif
namespace nvidia
{
namespace software
{
namespace updater
{
class CECDevice : public fpga_ceccommonutils::Util
{
    std::string name, inventoryPath;

  public:
    /**
     * @brief Construct a new CECDevice object
     *
     * @param objPath
     * @param busN
     * @param address
     * @param name
     */
    CECDevice(const std::string& objPath, uint8_t busN, uint8_t address,
              const std::string& name) :
        name(name),
        inventoryPath(objPath)
    {
        b = busN;
        d = address;
    }
    /**
     * @brief Get the Inventory Path object
     *
     * @return const std::string&
     */
    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }
};
/**
 * @brief concrete class for FPGA
 * @author
 * @since Wed Sept 13 2021
 */
class FPGAItemUpdater : public BaseItemUpdater
{
    std::vector<std::unique_ptr<CECDevice>> invs;

  public:
    /**
     * @brief Construct a new FPGAItemUpdater object
     *
     * @param bus
     */
    FPGAItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, FPGA_SUPPORTED_MODEL, FPGA_INVENTORY_IFACE, "FPGA",
                        FPGA_BUSNAME_UPDATER, FPGA_UPDATE_SERVICE, false,
                        FPGA_BUSNAME_INVENTORY)
    {
        nlohmann::json fruJson = fpga_ceccommonutils::loadJSONFile(
            "/usr/share/nvidia-cec/cec_config.json");
        if (fruJson == nullptr)
        {
            log<level::ERR>("InternalFailure when parsing the JSON file");
            return;
        }
        for (const auto& fru : fruJson.at("CEC"))
        {
            try
            {
                const auto baseinvInvPath =
                    "/xyz/openbmc_project/inventory/system/board/cec";
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
                    std::make_unique<CECDevice>(invpath, busId, devAddr, id);
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
        getServiceArgs([[maybe_unused]] const std::string& inventoryPath,
                       const std::string& imagePath,
                       const std::string& version,
                       const TargetFilter /* &targetFilter */,
                       [[maybe_unused]] const bool /* forceUpdate */) const override
    {

        // The systemd unit shall be escaped
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
