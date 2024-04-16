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

#pragma once

#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace phosphor::logging;

namespace nvidia
{
namespace mock
{
namespace common
{
inline json loadJSONFile([[maybe_unused]] const char* path)
{
    json data;
    return data;
}
/**
 * @brief Mock class for Utility
 *
 */
class Util
{
  protected:
    uint8_t b, d, m, c;

  public:
    virtual ~Util() = default;

    virtual bool getPresence() const
    {
        return true;
    }

    virtual std::string getSerialNumber() const
    {
        return "";
    }
    virtual std::string getPartNumber() const
    {
        return "";
    }
    virtual std::string getManufacturer() const
    {
        return "";
    }
    virtual std::string getModel() const
    {
        return "";
    }
    virtual std::string getVersion() const
    {
        return "";
    }
};
} // namespace common
} // namespace mock
} // namespace nvidia
