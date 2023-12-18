
#include "orin_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{
std::string ORINItemUpdater::getVersion([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string
    ORINItemUpdater::getManufacturer([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string ORINItemUpdater::getModel([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

} // namespace updater
} // namespace software
} // namespace nvidia
