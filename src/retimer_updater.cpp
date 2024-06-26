/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "config.h"

#include "retimer_updater.hpp"

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
    ReTimerItemUpdater::getVersion(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            std::string swPath =
                (boost::format(RT_SW_VERSION_PATH) % inv->getId()).str();
            try
            {
                ret = getProperty<std::string>(RT_BUSNAME_INVENTORY,
                                               swPath.c_str(), VERSION_IFACE,
                                               VERSION);
            }
            catch (const std::exception& e)
            {
                // ignore the exception for retimer
            }
        }
    }
    return ret;
}

std::string ReTimerItemUpdater::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    // GPU manager inventory does not implement manufacturer, return empty value
    return "";
}

std::string ReTimerItemUpdater::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    // GPU manager inventory does not implement model, return empty value
    return "";
}
std::string ReTimerItemUpdater::getSKU() const
{
    if (invs.empty())
    {
        return {};
    }

    std::string swPath = std::string(RT_INVENTORY_PATH) + invs.at(0)->getId();
    std::string ret{};
    try
    {
        ret = getProperty<std::string>(RT_BUSNAME_INVENTORY,
                                       swPath.c_str(), ASSET_IFACE,
                                       "SKU");
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(e.what());
    }
    return ret;
}
} // namespace updater
} // namespace software
} // namespace nvidia
