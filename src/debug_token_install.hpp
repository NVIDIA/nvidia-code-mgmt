
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <sstream>

namespace nvidia
{
namespace software
{
namespace updater
{

const uint8_t debugTokenInstall = 1;

/**
 * @brief Debug token install item updater
 * @author
 * @since Wed Aug 04 2021
 */
class DebugTokenInstallItemUpdater : public BaseItemUpdater
{

  public:
    /**
     * @brief Construct a new Debug token Install ItemUpdater object
     *
     * @param bus
     */
    DebugTokenInstallItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, DEBUG_TOKEN_INSTALL_SUPPORTED_MODEL,
                        DEBUG_TOKEN_INVENTORY_IFACE, DEBUG_TOKEN_INSTALL_NAME,
                        DEBUG_TOKEN_INSTALL_BUSNAME_UPDATER,
                        DEBUG_TOKEN_UPDATE_SERVICE, false,
                        DEBUG_TOKEN_BUSNAME_INVENTORY)
    {}
    /**
     * @brief Get the Version
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string
        getManufacturer(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(const std::string& inventoryPath) const override;
    /**
     * @brief Get service arguments
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath, const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter,
        [[maybe_unused]] const bool forceUpdate) const override
    {

        // The systemd unit shall be escaped
        std::string args = "";
        args += "\\x20";
        args += std::to_string(debugTokenInstall);
        args += "\\x20";
        args += version;
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
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
            std::string(SOFTWARE_OBJPATH) + "/" + DEBUG_TOKEN_INSTALL_NAME;
        ret.emplace_back(invPath);
        return ret;
    }
    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for debug tokens inventory is not supported
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
