/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gpio_monitor_read.hpp"

#include <cctype>
#include <map>
#include <variant>

namespace pwrseq::gpio_mon
{
namespace
{

constexpr const char* kGpioService = "xyz.openbmc_project.Gpio.Monitor";
constexpr const char* kLineBasePath = "/xyz/openbmc_project/gpio/monitor/lines";
constexpr const char* kLineInterface = "xyz.openbmc_project.Gpio.Monitor.Line";
constexpr const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";

using PropertyValue = std::variant<bool, std::string>;

const bool* boolProperty(const std::map<std::string, PropertyValue>& props,
                         const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return nullptr;
    }
    return std::get_if<bool>(&it->second);
}

} // namespace

std::string lineObjectPath(const std::string& lineName)
{
    // Mirrors phosphor-gpio-monitor's sanitizeGpioPathElement: a path
    // element may only hold [A-Za-z0-9_].
    std::string element;
    element.reserve(lineName.size());
    for (unsigned char ch : lineName)
    {
        const bool valid = std::isalnum(ch) != 0 || ch == '_';
        element.push_back(valid ? static_cast<char>(ch) : '_');
    }
    if (element.empty())
    {
        element = "line";
    }
    return std::string(kLineBasePath) + "/" + element;
}

std::optional<bool> readLineLevel(sdbusplus::bus::bus& bus,
                                  const std::string& lineName,
                                  uint64_t timeoutUsec)
{
    if (lineName.empty())
    {
        return std::nullopt;
    }

    const std::string path = lineObjectPath(lineName);
    std::map<std::string, PropertyValue> props;
    try
    {
        auto method = bus.new_method_call(kGpioService, path.c_str(),
                                          kPropertiesInterface, "GetAll");
        method.append(kLineInterface);
        auto reply = bus.call(method, timeoutUsec);
        reply.read(props);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    const bool* bound = boolProperty(props, "Bound");
    const bool* value = boolProperty(props, "Value");
    if (bound == nullptr || !*bound || value == nullptr)
    {
        return std::nullopt;
    }
    return *value;
}

} // namespace pwrseq::gpio_mon
