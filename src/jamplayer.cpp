#include "config.h"
#include "jamplayer.hpp"
#include <boost/format.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string
    JamPlayer::getVersion([
	[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string JamPlayer::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string JamPlayer::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
