/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "mtd_updater.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include <filesystem>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string MTDItemUpdater::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    std::string ret = "";
    if (softwareVersionObj)
    {
        std::ifstream file(copyPath, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr
                << "Error: Could not open image file to pick up the version"
                << copyPath << std::endl;
            return "";
        }

        file.seekg(versionOffset, std::ios::beg);
        if (!file.good())
        {
            std::cerr
                << "Error: Could not seek to offset on the image file to grab the version"
                << std::hex << versionOffset << std::endl;
            file.close();
            return "";
        }

        std::vector<char> buffer(versionSize);
        file.read(buffer.data(), versionSize);
        if (!file.good())
        {
            std::cerr << "Error: Could not read data at offset " << std::hex
                      << versionOffset << " to pick up the version"
                      << std::endl;
            file.close();
            return "";
        }

        file.close();

        std::stringstream ss;
        for (const auto& byte : buffer)
        {
            ss << std::hex << std::setw(2) << std::setfill('0')
               << (static_cast<unsigned>(byte) & 0xFF);
        }

        softwareVersionObj->version(ss.str());
        ret = ss.str();
    }
    return ret;
}

std::string MTDItemUpdater::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string MTDItemUpdater::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
