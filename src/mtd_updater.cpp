
#include "config.h"

#include "mtd_updater.hpp"

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
    MTDItemUpdater::getVersion([
	[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string MTDItemUpdater::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string MTDItemUpdater::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
