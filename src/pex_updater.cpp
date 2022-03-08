
#include "config.h"

#include "pex_updater.hpp"

#include <boost/algorithm/string.hpp>
#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string PEXItemUpdater::getVersion(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "3";
}

std::string
    PEXItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "Advanced Micro Devices, Inc";
}

std::string PEXItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "AMD EPYC 7742 64-Core Processor";
}
} // namespace updater
} // namespace software
} // namespace nvidia
