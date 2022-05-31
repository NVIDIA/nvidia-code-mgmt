#pragma once

#include <string>
#include <vector>

class ActivationListener
{
  public:
    /**
     * @brief
     * @param
     * @return
     */
    virtual ~ActivationListener() = default;

    /**
     * @brief Call back called by Version object to update about update status
     * to ItemUpdaters
     *
     * @param versionId
     * @param inventoryPath
     */
    virtual void onUpdateDone(const std::string& versionId,
                              const std::string& inventoryPath) = 0;
};

/**
 * @brief Utility super class for ItemUpdater
 * @since Wed Aug 04 2021
 */
class ItemUpdaterUtils
{
  public:
    /**
     * @brief Destroy the Item Updater Utils object
     *
     */
    virtual ~ItemUpdaterUtils() = default;

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */
    virtual std::vector<std::string> getItemUpdaterInventoryPaths() = 0;

    /**
     * @brief Get the Service Name object
     *
     * @return std::string
     */
    virtual std::string getServiceName() const = 0;

    /**
     * @brief Indicates wheather to do update all together at once
     *
     * @return true
     * @return false
     */
    virtual bool updateAllTogether() const = 0;

    /**
     * @brief Get the Update Service With Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @return std::string
     */
    virtual std::string
        getUpdateServiceWithArgs(const std::string& inventoryPath,
                                 const std::string& imagePath) const = 0;

    /**
     * @brief Get the Name object
     *
     * @return std::string
     */
    virtual std::string getName() const = 0;

    /**
     * @brief read the existing f/w
     *
     */
    virtual void readExistingFirmWare() = 0;

    /**
     * @brief Get D-Bus service name
     *
     * @param const char* path - D-Bus object path
     * @param const char* interface - D-Bus interface
     * @return std::string
     */
    virtual std::string getDbusService(const std::string& path,
                          const std::string& interface) = 0;
};
