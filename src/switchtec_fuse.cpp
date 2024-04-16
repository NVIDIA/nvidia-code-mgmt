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
#include "switchtec_fuse.hpp"
#include <boost/format.hpp>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string
    SwitchtecFuse::getVersion([
	[maybe_unused]] const std::string& inventoryPath) const
{
    const char* command = "switchtec mfg info /dev/i2c-27@0x10 | grep \"Secure State\"";
    std::string result = "";
    FILE* pipe = popen(command, "r");

    if (pipe) {
        char buffer[128];
        while (!feof(pipe)) {
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result += buffer;
            }
        }
        pclose(pipe);
    }
    if (result.empty()) {
        result = "make sure platform is ON";
    }
	if (softwareVersionObj)
		softwareVersionObj->version(result);

    return result;
}

std::string SwitchtecFuse::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string SwitchtecFuse::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}
} // namespace updater
} // namespace software
} // namespace nvidia
