
#include "config.h"

#include "fpga_updater.hpp"

#include <fmt/format.h>

#include <boost/algorithm/string.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string FPGAItemUpdater::getVersion(const std::string& inventoryPath) const
{
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            return inv->getVersion();
        }
    }
    return "";
}

std::string
    FPGAItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "NVIDIAManufacture";
}

std::string FPGAItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "CECModel";
}
} // namespace updater
} // namespace software
} // namespace nvidia
