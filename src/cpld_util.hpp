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
#include "xyz/openbmc_project/Common/error.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace phosphor::logging;

namespace nvidia::cpld::common
{
/**
 * @brief Loads json files
 *
 * @param path
 * @return json
 */

inline json loadJSONFile(const char* path)
{
    std::ifstream ifs(path);

    if (!ifs.good())
    {
        log<level::ERR>(std::string("Unable to open file "
                                    "PATH=" +
                                    std::string(path))
                            .c_str());
        return nullptr;
    }
    auto data = json::parse(ifs, nullptr, false);
    if (data.is_discarded())
    {
        log<level::ERR>(std::string("Failed to parse json "
                                    "PATH=" +
                                    std::string(path))
                            .c_str());
        return nullptr;
    }
    return data;
}

/**
 * @brief Get the Command object
 *
 * @return std::string
 */

inline std::string getCommand()
{
    return "";
}

/**
 * @brief Get the Command object
 *
 * @tparam T
 * @tparam Types
 * @param arg1
 * @param args
 * @return std::string
 */

template <typename T, typename... Types>
inline std::string getCommand(T arg1, Types... args)
{
    std::string cmd = " " + arg1 + getCommand(args...);

    return cmd;
}

/**
 * @brief Executes command on shell
 *
 * @tparam T
 * @tparam Types
 * @param path
 * @param args
 * @return std::vector<std::string>
 */

template <typename T, typename... Types>
inline std::vector<std::string> executeCmd(T&& path, Types... args)
{
    std::vector<std::string> stdOutput;
    std::array<char, 128> buffer;

    std::string cmd = path + getCommand(args...);

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                  pclose);
    if (!pipe)
    {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        stdOutput.emplace_back(buffer.data());
    }

    return stdOutput;
}

/**
 * @brief invalid chars
 *
 * @param char
 * @return bool
 */

inline bool invalidChar(char c)
{
    return !std::isprint(static_cast<unsigned char>(c));
}

/**
 * @brief erases nonprintable chars
 *
 * @param str
 */

inline void stripUnicode(std::string& str)
{
    str.erase(std::remove_if(str.begin(), str.end(), invalidChar), str.end());
}

/**
 * @brief Util
 *
 */

class Util
{
  protected:
    uint8_t b, d, m, c;

  public:
    virtual ~Util() = default;

    //   protected:
    virtual bool getPresence() const
    {
        return true;
    }

    /**
     * @brief Runs the shell command
     *
     * @param command , size
     * @return std::string
     */

    virtual std::string runCommand(uint8_t command, size_t size) const
    {
        std::string s = "";
        try
        {
            std::string cmd =
                fmt::format("cpldi2ccmd.sh {0:d}-{1:d}-{2:#04x}-{3:#04x}-{4:d}",
                            0, b, d, command, size);
            auto output = executeCmd(cmd);
            for (const auto& i : output)
                s += i;
        }
        catch (const std::exception& e)
        {
            std::cout << e.what() << std::endl;
        }
        std::cerr << command << " =  " << s << std::endl;
        stripUnicode(s);
        return s;
    }
    /**
     * @brief Get the Serial Number object
     *
     * @return std::string
     */

    virtual std::string getSerialNumber() const
    {
        return runCommand(0x9e, 18);
    }

    /**
     * @brief Get the Part Number object
     *
     * @return std::string
     */

    virtual std::string getPartNumber() const
    {
        return runCommand(0xad, 21);
    }

    /**
     * @brief Get the Manufacturer object
     *
     * @return std::string
     */

    virtual std::string getManufacturer() const
    {
        return runCommand(0x99, 11);
    }

    /**
     * @brief Get the Model object
     *
     * @return std::string
     */

    virtual std::string getModel() const
    {
        return runCommand(0x9a, 11);
    }

    /**
     * @brief Get the Version object
     *
     * @return std::string
     */

    virtual std::string getVersion() const
    {
        return runCommand(0x2d, 2);
    }
};
} // namespace nvidia::cpld::common
