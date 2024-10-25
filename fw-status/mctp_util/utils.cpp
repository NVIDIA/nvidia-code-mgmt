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

#include "utils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <sstream>

namespace utils
{

void printBuffer(bool isTx, const std::vector<uint8_t>& buffer, uint8_t eid)
{
    if (!buffer.empty())
    {
        std::ostringstream tempStream;
        for (int byte : buffer)
        {
            tempStream << std::setfill('0') << std::setw(2) << std::hex << byte
                       << " ";
        }
        if (isTx)
        {
            lg2::info("EID: {EID} Tx: {TX}", "EID", eid, "TX",
                      tempStream.str());
        }
        else
        {
            lg2::info("Rx: {RX}", "RX", tempStream.str());
        }
    }
}

void printBuffer(bool isTx, const std::vector<uint8_t>& buffer)
{
    if (!buffer.empty())
    {
        std::ostringstream tempStream;
        for (int byte : buffer)
        {
            tempStream << std::setfill('0') << std::setw(2) << std::hex << byte
                       << " ";
        }
        if (isTx)
        {
            lg2::info("Tx: {TX}", "TX", tempStream.str());
        }
        else
        {
            lg2::info("Rx: {RX}", "RX", tempStream.str());
        }
    }
}

} // namespace utils