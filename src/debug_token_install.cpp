
#include "config.h"

#include "debug_token_install.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

std::string DebugTokenInstallItemUpdater::getVersion([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string DebugTokenInstallItemUpdater::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string DebugTokenInstallItemUpdater::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
