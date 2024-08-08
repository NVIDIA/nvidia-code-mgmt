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

#include <stdbool.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <cstdint>
#include <vector>

namespace utils
{

/** @struct CustomFD
 *
 *  RAII wrapper for file descriptor.
 */
struct CustomFD
{
    CustomFD(const CustomFD&) = delete;
    CustomFD& operator=(const CustomFD&) = delete;
    CustomFD(CustomFD&&) = delete;
    CustomFD& operator=(CustomFD&&) = delete;

    CustomFD(int fd) : fd(fd)
    {}

    ~CustomFD()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }

    int operator()() const
    {
        return fd;
    }

  private:
    int fd = -1;
};

constexpr bool Tx = true;
constexpr bool Rx = false;

/** @brief Print the buffer
 *
 *  @param[in] isTx - True if the buffer is an outgoing MCTP VDM message,
                       false if the buffer is an incoming MCTP VDM message
 *  @param[in] buffer - Buffer to print
 *
 */
void printBuffer(bool isTx, const std::vector<uint8_t>& buffer);

/**
 *  @class DBusHandler
 *
 *  Wrapper class to handle the D-Bus calls
 *
 */
class DBusHandler
{
  public:
    /** @brief Get the asio connection. */
    static auto& getAsioConnection()
    {
        static boost::asio::io_context io;
        static auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        return conn;
    }
};

} // namespace utils