
#pragma once
#include "config.h"

#include "base_item_updater.hpp"

#include <filesystem>
#include <sstream>

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief GPU OCP recovery item updater
 * @author
 * @since Wed Aug 04 2021
 */
class OCPRecovery : public BaseItemUpdater
{

  public:
    /**
     * @brief Construct a new GPU OCP recovery ItemUpdater object
     *
     * @param bus
     */
    OCPRecovery(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, GPU_OCP_RECOVERY_SUPPORTED_MODEL,
                        GPU_OCP_RECOVERY_INVENTORY_IFACE, GPU_OCP_RECOVERY_NAME,
                        GPU_OCP_RECOVERY_BUSNAME_UPDATER,
                        GPU_OCP_RECOVERY_UPDATE_SERVICE, false,
                        GPU_OCP_RECOVERY_BUSNAME_INVENTORY)
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
     * @brief Fetch path to update file transferred by PLDM
     *
     * @param dirPath
     * @return std::string
     */
    std::string getUpdateFilePath(const std::string& dirPath) const
    {
        auto iter = std::filesystem::directory_iterator(dirPath);

        if (iter != std::filesystem::end(iter))
        {
            return iter->path();
        }
        log<level::ERR>("Directory does not contain any files");
        return "";
    }

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
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {
        // Pass the base path to the recovery tool
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
        std::vector<std::string> ret{};
        std::string invPath =
            std::string(SOFTWARE_OBJPATH) + "/" + GPU_OCP_RECOVERY_NAME;
        ret.emplace_back(invPath);
        return ret;
    }
    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return false - for gpu ocp recovery inventory is not supported
     */
    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief Get the Paths To Monitor object
     *
     * @return std::vector<std::filesystem::path>
     */
    virtual std::vector<std::filesystem::path>
        getPathsToMonitor() const override
    {
        std::vector<std::filesystem::path> pathsToMonitor{};
        for (const auto& [uuid, _] : deviceIds)
        {
            std::filesystem::path basePathToWatch(getImageUploadDir());
            basePathToWatch /= uuid;
            std::filesystem::path fspPathToWatch =
                basePathToWatch / GPU_OCP_FSP_COMP_ID;
            std::filesystem::path buildInfoPathToWatch =
                basePathToWatch / GPU_OCP_BUILD_INFO_COMP_ID;
            std::filesystem::path oobPathToWatch =
                basePathToWatch / GPU_OCP_OOBHUB_COMP_ID;
            std::filesystem::path fspRtPathToWatch =
                basePathToWatch / GPU_OCP_FSP_RT_COMP_ID;
            // Order: FSP FMC (0), BUILD_INFO (1), OOBHUB (2), FSP RT (3)
            pathsToMonitor.push_back(fspPathToWatch);
            pathsToMonitor.push_back(buildInfoPathToWatch);
            pathsToMonitor.push_back(oobPathToWatch);
            pathsToMonitor.push_back(fspRtPathToWatch);
        }
        if (pathsToMonitor.size() < 1)
        {
            // Fail
            throw std::runtime_error("No " + getName() + " to monitor");
        }
        return pathsToMonitor;
    }

    /**
     * @brief Process images,
     *          1) Creates Unique ID for dbus object
     *          2) calls initiates update
     *
     * @param filePath
     * @return int
     */
    virtual int processImage(std::filesystem::path& filePath) override;
};

} // namespace updater
} // namespace software
} // namespace nvidia
