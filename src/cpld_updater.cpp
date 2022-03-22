
#include "config.h"

#include "cpld_updater.hpp"

#include <fmt/format.h>

#include <boost/algorithm/string.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string CPLDItemUpdater::getVersion(const std::string& inventoryPath) const
{

    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            return inv->getVersion();
        }
    }
    return "";
    // return "01_30";
}

std::string
    CPLDItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "NVIDIAManufacture";
}

std::string CPLDItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "CPLDModel";
}
} // namespace updater
} // namespace software
} // namespace nvidia
