
#include "config.h"

#include "retimer_updater.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string
    ReTimerItemUpdater::getVersion(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            std::string swPath = (boost::format(RT_SW_VERSION_PATH)
                % inv->getId()).str();
            try
            {
                ret = getProperty<std::string>(
                    RT_BUSNAME_INVENTORY,
                    swPath.c_str(),
                    VERSION_IFACE, VERSION);
            }
            catch(const std::exception& e)
            {
                log<level::ERR>("GetVersion failed",
                    entry("ERROR=%s", e.what()));
            }
        }
    }
    return ret;
}

std::string
    ReTimerItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            try
            {
                ret = getProperty<std::string>(
                    RT_BUSNAME_INVENTORY,
                    (RT_INVENTORY_PATH + inv->getId()).c_str(),
                    ASSET_IFACE, MANUFACTURER);
            }
            catch(const std::exception& e)
            {
                log<level::ERR>("GetManufacturer failed",
                    entry("ERROR=%s", e.what()));
            }
        }
    }
    return ret;
}

std::string ReTimerItemUpdater::getModel(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            try
            {
                ret = getProperty<std::string>(
                    RT_BUSNAME_INVENTORY,
                    (RT_INVENTORY_PATH + inv->getId()).c_str(),
                    ASSET_IFACE, MODEL);
            }
            catch(const std::exception& e)
            {
                log<level::ERR>("GetModel failed",
                    entry("ERROR=%s", e.what()));
            }
        }
    }
    return ret;
}
} // namespace updater
} // namespace software
} // namespace nvidia
