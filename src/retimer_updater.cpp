
#include "config.h"

#include "retimer_updater.hpp"

#include <boost/algorithm/string.hpp>

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
            ret = inv->getVersion();
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
            ret = inv->getManufacturer();
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
            ret = inv->getModel();
        }
    }
    return ret;
    ;
}
} // namespace updater
} // namespace software
} // namespace nvidia
