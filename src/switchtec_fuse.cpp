#include "config.h"
#include "switchtec_fuse.hpp"
#include <boost/format.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string
    SwitchtecFuse::getVersion([
	[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string SwitchtecFuse::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string SwitchtecFuse::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
