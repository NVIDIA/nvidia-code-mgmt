
#pragma once
#include "config.h"

#include "base_item_updater.hpp"
#include "dbusutils.hpp"

#include <sdbusplus/bus/match.hpp>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto inventoryRootPath = "/xyz/openbmc_project/inventory";
constexpr auto spiObjectInterfaces = "xyz.openbmc_project.Configuration.SPI";

namespace MatchRules = sdbusplus::bus::match::rules;

namespace nvidia
{
namespace software
{
namespace updater
{
class SPIProgrammer : public BaseItemUpdater
{
  public:
    /**
     * @brief Construct a new SPI Programmer Item Updater object
     *
     * @param bus dbus reference
     */
    SPIProgrammer(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, SPI_PROGRAMMER_SUPPORTED_MODEL,
                        SPI_PROGRAMMER_INVENTORY_IFACE, SPI_PROGRAMMER_NAME,
                        SPI_PROGRAMMER_BUSNAME_UPDATER,
                        SPI_PROGRAMMER_UPDATE_SERVICE, true,
                        SPI_PROGRAMMER_BUSNAME_INVENTORY)
    {
        // Try to populate SPI Software objects as EM objects might not be
        // present at the time of creation
        tryPopulateSpiSwObjects();
    }

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

    std::string getIdProperty(const std::string& identifier) override
    {
        return identifier;
    }

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
        // put all the SPI chip names in the args
        for (const auto& [inventoryPath, pair] : inventoryMap)
        {
            auto [chip, fwTarget] = pair;
            args += chip;
            args += "\\x20";
        }
        args += "\\x20";
        args += imagePath;
        std::replace(args.begin(), args.end(), '/', '-');
        return args;
    }

    std::string getString(const InterfaceMap& interfaces,
                          const Interface& interface, const Property& property)
    {
        try
        {
            return std::get<std::string>(interfaces.at(interface).at(property));
        }
        catch (std::exception& e)
        {
            lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                       "ERR", e.what());
            return {};
        }
    }
    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> paths;
        for (const auto& [inventoryPath, pair] : inventoryMap)
        {
            paths.push_back(inventoryPath);
        }
        lg2::info("SPIProgrammer:: Found {COUNT} inventory paths", "COUNT",
                  paths.size());
        return paths;
    }

    /**
     * @brief Get timeout for SPI programmer
     *
     * @return uint32_t
     */
    uint32_t getTimeout() override
    {
        lg2::info("Timeout return {TIMEOUT}", "TIMEOUT",
                  SPI_PROGRAMMER_TIMEOUT);
        return SPI_PROGRAMMER_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for SPI programmer inventory check is not required
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }

  private:
    // inventoryPath, (chip name, targetFirmware)
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        inventoryMap;

    // D-Bus match object for delayed initialization
    std::unique_ptr<sdbusplus::bus::match_t> inventoryObjectMatch;

    /**
     * @brief Check if SPI objects are present in Entity Manager
     *
     * @return bool True if SPI objects are found, false otherwise
     */
    bool isSpiObjectPresent();

    /**
     * @brief Populate SPI Software objects from Entity Manager configuration
     *
     * @return void
     */
    void populateSpiSwObjects();

    /**
     * @brief Try populating SPI Software objects, with fallback to D-Bus match
     * rule if no SPI objects are present
     *
     * @return void
     */
    void tryPopulateSpiSwObjects();
};

} // namespace updater
} // namespace software
} // namespace nvidia
