
#include "config.h"

#include "glacier_crisis_recovery.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{
std::string GlacierRecovery::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string GlacierRecovery::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string GlacierRecovery::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
