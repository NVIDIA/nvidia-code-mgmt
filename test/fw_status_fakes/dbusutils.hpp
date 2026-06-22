#pragma once

#include <sdbusplus/bus.hpp>

#include <cerrno>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace nvidia::software::updater
{

using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;
using Interface = std::string;
using Property = std::string;
using ObjectPath = std::string;
using Interfaces = std::vector<std::string>;
using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using MapperServiceMap = std::vector<std::pair<std::string, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;
using ObjectValueTree = std::map<ObjectPath, InterfaceMap>;

} // namespace nvidia::software::updater

namespace test::fw_status_fake_dbus
{

using Value = nvidia::software::updater::Value;
using InterfaceMap = nvidia::software::updater::InterfaceMap;
using ObjectValueTree = nvidia::software::updater::ObjectValueTree;
using GetSubTreeResponse = nvidia::software::updater::GetSubTreeResponse;

inline ObjectValueTree managedObjects{};
inline std::string hostPowerStatus{};
inline std::vector<std::string> startedUnits{};
inline std::vector<std::string> restartedUnits{};
inline std::set<std::tuple<std::string, std::string, std::string>>
    throwingProperties{};
inline bool throwOnGetManagedObjects = false;

template <typename T>
inline void setProperty(const std::string& path, const std::string& interface,
                        const std::string& property, T value)
{
    managedObjects[path][interface][property] = std::move(value);
}

inline void throwProperty(const std::string& path, const std::string& interface,
                          const std::string& property)
{
    throwingProperties.emplace(path, interface, property);
}

inline void reset()
{
    managedObjects.clear();
    hostPowerStatus.clear();
    startedUnits.clear();
    restartedUnits.clear();
    throwingProperties.clear();
    throwOnGetManagedObjects = false;
}

} // namespace test::fw_status_fake_dbus

namespace nvidia::software::updater
{

class DBUSUtils
{
  public:
    explicit DBUSUtils(sdbusplus::bus_t&)
    {}

    template <typename T>
    T getProperty(const char*, const char* path, const char* interface,
                  const char* propertyName) const
    {
        const std::string pathKey = path ? path : "";
        const std::string interfaceKey = interface ? interface : "";
        const std::string propertyKey = propertyName ? propertyName : "";

        if (test::fw_status_fake_dbus::throwingProperties.contains(
                std::make_tuple(pathKey, interfaceKey, propertyKey)))
        {
            throw sdbusplus::exception::SdBusError(EIO,
                                                   "fake property failure");
        }

        return std::get<T>(test::fw_status_fake_dbus::managedObjects.at(pathKey)
                               .at(interfaceKey)
                               .at(propertyKey));
    }

    ObjectValueTree getManagedObjects(const char*, const char*) const
    {
        if (test::fw_status_fake_dbus::throwOnGetManagedObjects)
        {
            return {};
        }
        return test::fw_status_fake_dbus::managedObjects;
    }

    std::string getHostPwrStatus() const
    {
        return test::fw_status_fake_dbus::hostPowerStatus;
    }

    void startSystemUnit(const std::string& unit) const
    {
        test::fw_status_fake_dbus::startedUnits.push_back(unit);
    }

    void restartSystemUnit(const std::string& unit) const
    {
        test::fw_status_fake_dbus::restartedUnits.push_back(unit);
    }
};

} // namespace nvidia::software::updater
