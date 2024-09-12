#include "config.h"

#include "vmeplayer.hpp"

#include <boost/format.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string VmePlayer::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string VmePlayer::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string
    VmePlayer::getModel([[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
