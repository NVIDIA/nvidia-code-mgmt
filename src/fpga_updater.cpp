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

#include "fpga_updater.hpp"

#include <fmt/format.h>

#include <boost/algorithm/string.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string FPGAItemUpdater::getVersion(const std::string& inventoryPath) const
{
    for (auto& inv : invs)
    {
        if (inv->getInventoryPath() == inventoryPath)
        {
            return inv->getVersion();
        }
    }
    return "";
}

std::string
    FPGAItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "NVIDIAManufacture";
}

std::string FPGAItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "CECModel";
}
} // namespace updater
} // namespace software
} // namespace nvidia
