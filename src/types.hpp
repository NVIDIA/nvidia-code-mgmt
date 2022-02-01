#pragma once

#include <string>
#include <tuple>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{

using AssociationList =
    std::vector<std::tuple<std::string, std::string, std::string>>;
}
} // namespace software
} // namespace nvidia
