
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief PEX concrete updater
 * @author
 * @since Wed Aug 04 2021
 */
class PEXItemUpdater : public BaseItemUpdater
{
  public:
    /**
     * @brief Construct a new PEXItemUpdater object
     *
     * @param bus
     */
    PEXItemUpdater(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, PEX_SUPPORTED_MODEL, PEX_INVENTORY_IFACE, "PEX",
                        PEX_BUSNAME_UPDATER, PEX_UPDATE_SERVICE, false,
                        PEX_BUSNAME_INVENTORY)
    {}
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
        std::string args = inventoryPath;
        args += "\\x20";
        args += imagePath;
        std::replace(args.begin(), args.end(), '/', '-');

        return args;
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
