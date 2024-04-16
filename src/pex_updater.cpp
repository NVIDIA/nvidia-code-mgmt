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

#include "pex_updater.hpp"

#include <boost/algorithm/string.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string PEXItemUpdater::getVersion(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "3";
}

std::string
    PEXItemUpdater::getManufacturer(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "Advanced Micro Devices, Inc";
}

std::string PEXItemUpdater::getModel(const std::string& inventoryPath) const
{
    (void)inventoryPath;
    return "AMD EPYC 7742 64-Core Processor";
}
} // namespace updater
} // namespace software
} // namespace nvidia
