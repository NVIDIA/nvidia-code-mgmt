
#include "config.h"

#include "psu_updater.hpp"

#include <boost/algorithm/string.hpp>
#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{

std::string PSUItemUpdater::getVersion(const std::string& inventoryPath) const
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
    PSUItemUpdater::getManufacturer(const std::string& inventoryPath) const
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

std::string PSUItemUpdater::getModel(const std::string& inventoryPath) const
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
}
} // namespace updater
} // namespace software
} // namespace nvidia
