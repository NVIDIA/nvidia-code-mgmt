/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

#pragma once

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace nvidia::software::updater
{

using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;
using PropertyMap = std::map<std::string, Value>;
using InterfaceMap = std::map<std::string, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::object_path, InterfaceMap>;

} // namespace nvidia::software::updater

namespace nvidia::software::updater
{

class DBUSUtils
{
  public:
    explicit DBUSUtils(sdbusplus::bus_t&)
    {}

    ObjectValueTree getManagedObjects(const char*, const char*) const noexcept
    {
        return {};
    }
};

} // namespace nvidia::software::updater
