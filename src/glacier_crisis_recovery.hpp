
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{
class GlacierRecovery : public BaseItemUpdater
{

  public:
    /**
     * @brief Construct a new Glacier Recovery Item Updater object
     *
     * @param bus dbus reference
     * @param together update everything together
     */
    GlacierRecovery(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, GLACIER_RECOVERY_SUPPORTED_MODEL,
                        GLACIER_RECOVERY_INVENTORY_IFACE, GLACIER_RECOVERY_NAME,
                        GLACIER_RECOVERY_BUSNAME_UPDATER,
                        GLACIER_RECOVERY_UPDATE_SERVICE, false,
                        GLACIER_RECOVERY_BUSNAME_INVENTORY)
    {}

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
    std::string getManufacturer(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(
        [[maybe_unused]] const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @param forceUpdate
     * @return std::string
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {

        // The systemd unit shall be escaped
        std::string args = "";
        args += "\\x20";
        args += imagePath;
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
            std::string(SOFTWARE_OBJPATH) + "/" + GLACIER_RECOVERY_NAME;
        ret.emplace_back(invPath);
        return ret;
    }

    /**
     * @brief Get timeout for glacier recovery
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        return GLACIER_RECOVERY_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for glacier recovery inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
