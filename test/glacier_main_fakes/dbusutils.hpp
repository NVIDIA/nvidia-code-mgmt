#pragma once

#include "sdbusplus/bus.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <variant>

namespace nvidia::software::updater
{

using Value = std::variant<bool, uint64_t>;
using Interface = std::string;
using Property = std::string;
using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<std::filesystem::path, InterfaceMap>;

inline ObjectValueTree fakeManagedObjects{};

class DBUSUtils
{
  public:
    explicit DBUSUtils(sdbusplus::bus_t&)
    {}

    template <typename T>
    T getProperty(const char*, const char* path, const char* interface,
                  const char* propertyName) const
    {
        return std::get<T>(fakeManagedObjects.at(std::filesystem::path(path))
                               .at(interface)
                               .at(propertyName));
    }

    ObjectValueTree getManagedObjects(const char*, const char*) const noexcept
    {
        return fakeManagedObjects;
    }
};

} // namespace nvidia::software::updater
