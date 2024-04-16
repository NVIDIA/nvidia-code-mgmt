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

#include "psu_updater.hpp"

#include <boost/algorithm/string.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{

std::string PSUItemUpdater::getVersion(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            ret = inv->getVersion();
        }
    }
    return ret;
}

std::string
    PSUItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            ret = inv->getManufacturer();
        }
    }
    return ret;
}

std::string PSUItemUpdater::getModel(const std::string& inventoryPath) const
{
    std::string ret = "";
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            ret = inv->getModel();
        }
    }
    return ret;
}
} // namespace updater
} // namespace software
} // namespace nvidia
