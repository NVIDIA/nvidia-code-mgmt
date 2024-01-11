#pragma once

#include <string>
#include <vector>
#include <sdbusplus/bus.hpp>

/**
 * @brief Enumeration for target filter types
 * 
 */
enum class TargetFilterType
{
    UpdateNone,
    UpdateSelected,
    UpdateAll
};

/**
 * @brief Target filter struct contains type of target filter and targets to
 * update.
 *
 */
struct TargetFilter
{
    TargetFilterType type;
    std::vector<std::string> targets;
};

class ActivationListener
{
  public:
    /**
     * @brief
     * @param
     * @return
     */
    virtual ~ActivationListener() = default;

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
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string
        getUpdateServiceWithArgs(const std::string& inventoryPath,
                                 const std::string& imagePath,
                                 const std::string& version,
                                 const TargetFilter& targetFilter,
                                 const bool forceUpdate) const = 0;

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
    
    /**
     * @brief apply target filters for non-pldm devices
     * 
     * @param targets 
     * @return TargetFilter 
     */
    virtual TargetFilter applyTargetFilters(
        const std::vector<sdbusplus::message::object_path>& targets) = 0;

    /**
     * @brief Get timeout for non-pldm devices
     *
     * @return uint32_t
     */
    virtual uint32_t getTimeout() = 0;

    /**
     * @brief method to check if inventory is supported, if inventory is not
     * supported then D-Bus calls to check compatibility can be ignored
     *
     * @return true - if inventory is supported else false
     */
    virtual bool inventorySupported() = 0;
};
