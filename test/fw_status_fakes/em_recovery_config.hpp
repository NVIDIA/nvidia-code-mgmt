#pragma once

#include "dbusutils.hpp"

namespace nvidia::software::updater::em_recovery
{

// Same test as src/em_recovery_config.hpp, against the fake InterfaceMap.
inline bool isConfigObject(const InterfaceMap& ifaces)
{
    if (ifaces.contains("xyz.openbmc_project.Configuration.USBDFURecovery"))
    {
        return true;
    }
    return ifaces.contains("xyz.openbmc_project.Configuration.NvDevice") &&
           ifaces.contains(
               "xyz.openbmc_project.Configuration.NvDevice.Recovery");
}

} // namespace nvidia::software::updater::em_recovery
