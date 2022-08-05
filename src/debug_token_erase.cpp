
#include "config.h"

#include "debug_token_erase.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

std::string DebugTokenEraseItemUpdater::getVersion([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string DebugTokenEraseItemUpdater::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string DebugTokenEraseItemUpdater::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
