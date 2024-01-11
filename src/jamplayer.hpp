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

class JamPlayer : public BaseItemUpdater
{   
  public:
    JamPlayer(sdbusplus::bus::bus& bus) :
		BaseItemUpdater(bus, JAMPLAYER_SUPPORTED_MODEL, JAMPLAYER_INVENTORY_IFACE, "JAMPLAYER",
						JAMPLAYER_BUSNAME_UPDATER, JAMPLAYER_SERVICE, false, JAMPLAYER_BUSNAME_INVENTORY)
    {
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
                       const std::string& version,
                       [[maybe_unused]] const TargetFilter &targetFilter,
                       [[maybe_unused]] const bool forceUpdate) const override
    {
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += version;
        args += "\\x20";
        args += "JAMPLAYER";
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
            std::string(SOFTWARE_OBJPATH) + "/JamPlayer";
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
        return JAMPLAYER_TIMEOUT;
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
