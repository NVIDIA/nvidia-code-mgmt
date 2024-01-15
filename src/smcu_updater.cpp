
#include "smcu_updater.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{
std::string SMCUItemUpdater::getVersion([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string
    SMCUItemUpdater::getManufacturer([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string SMCUItemUpdater::getModel([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

} // namespace updater
} // namespace software
} // namespace nvidia
