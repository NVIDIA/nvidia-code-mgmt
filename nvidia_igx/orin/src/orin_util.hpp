/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace nvidia::orin::common
{

inline std::string readVersionFile(const std::string& cmd)
{
    std::string output = "";
    std::array<char, 128> buffer;

    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
    {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        output += buffer.data();
    }

    // Remove trailing newline if present
    if (!output.empty() && output.back() == '\n')
    {
        output.pop_back();
    }

    return output;
}

class Util
{
  public:
    virtual ~Util() = default;

    virtual std::string getVersion() const
    {
        std::string version = "";
        try
        {
            std::string cmd = "cat /var/emmc/firmware-storage/version.txt";
            version = readVersionFile(cmd);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to fetch version: ", "ERROR", e.what());
        }

        return version;
    }

    virtual std::string getPlatformName() const
    {
        try
        {
            std::string cmd = "/usr/bin/igx-platform-detection.py";
            std::string platform = readVersionFile(cmd);
            return platform.empty() ? "Host" : platform;
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to fetch platform name: ", "ERROR", e.what());
            return "Host";
        }
    }
};

} // namespace nvidia::orin::common
