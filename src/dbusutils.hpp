
#pragma once
#include "types.hpp"
#include "watch.hpp"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <cstring>
#include <experimental/any>
#include <filesystem>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{
// Due to a libstdc++ bug, we got compile error using std::any with gmock.
// A temporary workaround is to use std::experimental::any.
// See details in https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90415
using std::experimental::any;
using std::experimental::any_cast;
using PropertyType = std::variant<std::string, bool>;

/**
 * @brief
 * @author
 * @since Wed Aug 04 2021
 */
class DBUSUtils
{

  public:
    /**
     * @brief Construct a new DBUSUtils object
     *
     * @param bus
     */
    DBUSUtils(sdbusplus::bus::bus& bus) : bus(bus)
    {}
    /**
     * @brief get inventory objects of interface
     *
     * @param iface
     * @return std::vector<std::string>
     */
    std::vector<std::string> getinventoryPath(const std::string& iface);

    /**
     * @brief Change the path is associated
     *
     * @param inventoryPath
     * @param assocs
     * @return true
     * @return false
     */
    bool isAssociated(const std::string& inventoryPath,
                      const AssociationList& assocs);
    /**
     * @brief Create a Version ID object
     *
     * @param updaterName
     * @param version
     * @return std::string
     */
    std::string createVersionID(const std::string& updaterName,
                                const std::string& version);

    /**
     * @brief Get the Property Impl object
     *
     * @param service
     * @param path
     * @param interface
     * @param propertyName
     * @return any
     */
    any getPropertyImpl(const char* service, const char* path,
                        const char* interface, const char* propertyName);

    /**
     * @brief Get the Property object
     *
     * @tparam T
     * @param service
     * @param path
     * @param interface
     * @param propertyName
     * @return T
     */
    template <typename T>
    T getProperty(const char* service, const char* path, const char* interface,
                  const char* propertyName)
    {
        any result = getPropertyImpl(service, path, interface, propertyName);
        auto value = any_cast<PropertyType>(result);
        return std::get<T>(value);
    }

    /**
     * @brief Get the Services object
     *
     * @param path
     * @param interface
     * @return std::vector<std::string>
     */
    std::vector<std::string> getServices(const char* path,
                                         const char* interface);
    /**
     * @brief Get the Service object
     *
     * @param path
     * @param interface
     * @return std::string
     */
    std::string getService(const char* path, const char* interface);

    /**
     * @brief Get the Software Objects object
     *
     * @return std::vector<std::string>
     */
    std::vector<std::string> getSoftwareObjects();

    /**
     * @brief
     *
     * @param objPath
     * @return true
     * @return false
     */
    bool findSoftwareObject(std::string& objPath);

  protected:
    sdbusplus::bus::bus& bus;

}; // DBUSUtils
} // namespace updater
} // namespace software
} // namespace nvidia