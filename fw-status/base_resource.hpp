#pragma once

#include "xyz/openbmc_project/Common/UUID/server.hpp"
#include "xyz/openbmc_project/State/Decorator/Health/server.hpp"
#include "xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp"
#include "com/nvidia/RoT/BootStatus/server.hpp"

#include "dbusutils.hpp"
#include <phosphor-logging/lg2.hpp>

#include <unordered_set>
#include <unordered_map>
#include <iostream>

using ResourceInterfacesInherit = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Common::server::UUID,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus
    >;

using BootStatusInterfaceInherit = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::RoT::server::BootStatus
    >;

using HealthServer = sdbusplus::xyz::openbmc_project::State::Decorator::server::Health;
using OperationalStatusServer = sdbusplus::xyz::openbmc_project::State::Decorator::server::OperationalStatus;
using BootStatusServer = sdbusplus::com::nvidia::RoT::server::BootStatus;

namespace MatchRules = sdbusplus::bus::match::rules;
using namespace phosphor::logging;

/**@class ResourceInterface
 *
 *  Concrete implementation of xyz.openbmc_project.Common.UUID,
 *  xyz.openbmc_project.State.Decorator.Health, and xyz.openbmc_project.State.Decorator.OperationalStatus
 *  D-Bus interfaces
 *
 */
class ResourceInterfaces : public ResourceInterfacesInherit
{
    public:
        ResourceInterfaces(sdbusplus::bus::bus& sdbus, const std::string& objPath) :
            ResourceInterfacesInherit(sdbus, objPath.c_str(), action::emit_interface_added)
        {
        }
};

/**@class BootStatus
 *
 *  Concrete implementation of com.nvidia.ERoT.BootStatus
 *  D-Bus interface
 *
 */
class BootStatus : public BootStatusInterfaceInherit
{
    public:
        BootStatus(sdbusplus::bus::bus& sdbus, const std::string& objPath) :
            BootStatusInterfaceInherit(sdbus, objPath.c_str(), action::emit_interface_added)
        {
        }
};

/**@class BaseResource
 *
 *  BaseResource represents a resource at the most abstract level and publishes the ResourceInterfaces
 *  expected from all resources
 *
 */
class BaseResource : public ResourceInterfaces
{
    public:
        sdbusplus::bus::bus& bus;

        /**@brief Constructor for the BaseResource Class
         *
         * @param bus - SystemD bus to publish the object
         * @param objPath - Path of D-Bus object to publish
         *
         */
        BaseResource(sdbusplus::bus::bus& bus, const std::string& objPath) :
            ResourceInterfaces(bus, objPath),
            bus(bus),
            path(objPath)
        {
        }

    protected:
        const std::string& path;
};
