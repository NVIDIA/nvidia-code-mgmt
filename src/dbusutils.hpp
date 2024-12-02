/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "watch.hpp"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <cstring>
#include <experimental/any>
#include <filesystem>
#include <string>
#include <typeinfo>

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
using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;
using Interface = std::string;
using Property = std::string;
using PropertyType = std::variant<std::string, bool>;
using ObjectPath = std::string;
using Interfaces = std::vector<std::string>;
using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
using MapperServiceMap = std::vector<std::pair<std::string, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;

const std::string hostOn = "xyz.openbmc_project.State.Chassis.PowerState.On";

namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

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
                        const char* interface, const char* propertyName) const;

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
                  const char* propertyName) const
    {
        any result = getPropertyImpl(service, path, interface, propertyName);
        Value value{};
        try
        {
            value = any_cast<Value>(result);
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "GetProperty failed. Unable to retrive property {PROPERTY}"
                " as type {TYPE}",
                "PROPERTY", propertyName, "TYPE", typeid(T).name());
            return T{};
        }
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
     * @brief Get the Managed Objects from an object manager
     *
     * @param service
     * @param objManagerPath
     * @return std::vector<std::string>
     */
    ObjectValueTree
        getManagedObjects(const char* service,
                          const char* objManagerPath) const noexcept;

    /**
     * @brief
     *
     * @param objPath
     * @return true
     * @return false
     */
    bool findSoftwareObject(std::string& objPath);

    /**
     * @brief Restart a systemd unit
     *
     * @param systemUnit
     */
    inline void restartSystemUnit(const std::string& systemUnit) const noexcept
    {
        controlSystemUnit(systemUnit, "RestartUnit");
    }

    /**
     * @brief Start a systemd unit
     *
     * @param systemUnit
     */
    inline void startSystemUnit(const std::string& systemUnit) const noexcept
    {
        controlSystemUnit(systemUnit, "StartUnit");
    }

    /**
     * @brief Get power status of Host
     */
    std::string getHostPwrStatus() const noexcept;

    /**
     * @brief Create a Log entry
     */
    void createLog(const std::string& messageID,
                   std::map<std::string, std::string>& addData,
                   Level& level) const;
    /**
     * @brief Create a Message Registry for Resource Event Errors
     */
    void createMessageRegistryResourceErrors(
        const std::string& messageID, const std::string& deviceName,
        const std::string& errorMsg, const std::string& resolution) const;

  protected:
    sdbusplus::bus::bus& bus;

  private:
    /**
     * @brief Control a systemd unit
     *
     * @param systemUnit
     * @param action
     */
    void controlSystemUnit(const std::string& systemUnit,
                           const std::string& action) const noexcept;

}; // DBUSUtils
} // namespace updater
} // namespace software
} // namespace nvidia
