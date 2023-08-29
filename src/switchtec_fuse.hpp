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

class SwitchtecFuse : public BaseItemUpdater
{   
  std::unique_ptr<SoftwareVersion> softwareVersionObj;
  public:
    SwitchtecFuse(sdbusplus::bus::bus& bus) :
        BaseItemUpdater(bus, SWITCHTEC_SUPPORTED_MODEL, SWITCHTEC_INVENTORY_IFACE, "PCIE_SWITCH_FUSE",
                        SWITCHTEC_BUSNAME_UPDATER, SWITCHTEC_FUSE_SERVICE, false, SWITCHTEC_BUSNAME_INVENTORY)
    {
        auto objPath = std::string(SOFTWARE_OBJPATH) + "/PCIE_SWITCH_FUSE";
        createInventory(bus, objPath);
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
                       [[maybe_unused]] const TargetFilter &targetFilter) const override
    {
        std::string args = "";
        args += "\\x20";
        args += imagePath;
        args += "\\x20";
        args += version;
        args += "\\x20";
        args += "SWITCHTEC_PCIE_SWITCH";
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
            std::string(SOFTWARE_OBJPATH) + "/Switchtec";
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
        return SWITCHTEC_FUSE_TIMEOUT;
    }

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false */
    bool inventorySupported() override
    {
        return false;
    }

    /**
     * @brief create version interface for required non-pldm devices
     * @param bus
     * @param objpath
     * @param versionId
     */
    void createInventory(sdbusplus::bus::bus& bus,
                                const std::string& objPath)
    {
        getVersion("");
        softwareVersionObj = std::make_unique<SoftwareVersion>(bus, objPath);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
