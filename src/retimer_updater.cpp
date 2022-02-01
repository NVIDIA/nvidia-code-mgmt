
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
    (void)inventoryPath;
    return "2";
}

std::string
    ReTimerItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "Micron_Technology";
}

std::string ReTimerItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "36ASF8G72PZ3G2B2";
}
} // namespace updater
} // namespace software
} // namespace nvidia
