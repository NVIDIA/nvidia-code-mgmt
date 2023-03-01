#pragma once
#include "config.h"

#include "base_item_updater.hpp"
#include "fstream"

namespace nvidia
{
namespace software
{
namespace updater
{

class MTDItemUpdater : public BaseItemUpdater
{
    std::string mtdName;
    
  public:
    MTDItemUpdater(sdbusplus::bus::bus& bus, std::string mtdN, std::string modelName) :
		BaseItemUpdater(bus, modelName, MTD_INVENTORY_IFACE, "MTD_FW_" + mtdN,
						MTD_BUSNAME_UPDATER_BASE + mtdN,
                        MTD_UPDATE_SERVICE, false, MTD_BUSNAME_INVENTORY_BASE + mtdN),
		mtdName(mtdN)

    {
    }

	static bool partitionExists(const std::string mtdName)
    {
        std::ifstream f("/proc/mtd");
        if (!f)
          return false;
        std::string line;
        std::string name = "\"";
        name.append(mtdName);
        name.append("\"");
        while (std::getline(f, line))
        {
            if (line.find(name) != std::string::npos) {
                f.close();
                return true;
            }
        }
        f.close();
        return false;
    }

    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion([
		[maybe_unused]] const std::string& inventoryPath) const override;

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
                       [[maybe_unused]] const std::string& version,
                       [[maybe_unused]] const TargetFilter &targetFilter) const override
    {
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += mtdName;
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        std::string invPath =
            std::string(SOFTWARE_OBJPATH) + "/" + mtdName;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout from config file for retimer
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return MTD_UPDATE_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for mtd inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
