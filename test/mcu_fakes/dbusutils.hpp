#pragma once

#include "sdbusplus/bus.hpp"

#include <cstdint>
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

using Value = std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t,
                           int64_t, uint64_t, double, std::string,
                           std::vector<uint8_t>, std::vector<std::string>>;
using Interface = std::string;
using Property = std::string;
using ObjectPath = std::string;
using Interfaces = std::vector<std::string>;
using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using MapperServiceMap = std::vector<std::pair<std::string, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;

} // namespace nvidia::software::updater

namespace test::mcu_fake_dbus
{

using Value = nvidia::software::updater::Value;
using InterfaceMap = nvidia::software::updater::InterfaceMap;
using GetSubTreeResponse = nvidia::software::updater::GetSubTreeResponse;

inline std::map<std::string, InterfaceMap> objectProperties{};
inline std::set<std::tuple<std::string, std::string, std::string>>
    throwingProperties{};

template <typename T>
inline void setProperty(const std::string& path, const std::string& interface,
                        const std::string& property, T value)
{
    objectProperties[path][interface][property] = std::move(value);
}

inline void throwProperty(const std::string& path, const std::string& interface,
                          const std::string& property)
{
    throwingProperties.emplace(path, interface, property);
}

inline void setSubTree(const GetSubTreeResponse& response)
{
    test::mcu_fake_bus::setReply(response);
}

inline void failSubTree(const std::string& message)
{
    test::mcu_fake_bus::setError(message);
}

inline void reset()
{
    objectProperties.clear();
    throwingProperties.clear();
    test::mcu_fake_bus::reset();
    test::mcu_fake_bus::resetReply<GetSubTreeResponse>();
}

} // namespace test::mcu_fake_dbus

namespace nvidia::software::updater
{

class DBUSUtils
{
  public:
    explicit DBUSUtils(sdbusplus::bus::bus&)
    {}

    template <typename T>
    T getProperty(const char*, const char* path, const char* interface,
                  const char* propertyName) const
    {
        if (path == nullptr)
        {
            throw std::invalid_argument("path is null");
        }
        if (interface == nullptr)
        {
            throw std::invalid_argument("interface is null");
        }
        if (propertyName == nullptr)
        {
            throw std::invalid_argument("propertyName is null");
        }

        const std::string pathKey = path;
        const std::string interfaceKey = interface;
        const std::string propertyKey = propertyName;
        const auto throwKey =
            std::make_tuple(pathKey, interfaceKey, propertyKey);
        if (test::mcu_fake_dbus::throwingProperties.contains(throwKey))
        {
            throw sdbusplus::exception::SdBusError("fake property failure");
        }

        auto pathIt = test::mcu_fake_dbus::objectProperties.find(pathKey);
        if (pathIt == test::mcu_fake_dbus::objectProperties.end())
        {
            throw sdbusplus::exception::SdBusError("path not found");
        }

        auto ifaceIt = pathIt->second.find(interfaceKey);
        if (ifaceIt == pathIt->second.end())
        {
            throw sdbusplus::exception::SdBusError("interface not found");
        }

        auto propIt = ifaceIt->second.find(propertyKey);
        if (propIt == ifaceIt->second.end())
        {
            throw sdbusplus::exception::SdBusError("property not found");
        }

        return std::get<T>(propIt->second);
    }
};

} // namespace nvidia::software::updater
