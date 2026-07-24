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

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nvidia::fw_status::boot_status
{

constexpr size_t ap0BootCompleteBit = 5;
constexpr size_t ap0BootCompleteTimeoutBit = 27;
constexpr size_t apFatalErrorCodeFirstBit = 28;
constexpr size_t apFatalErrorCodeBitCount = 4;

inline bool getBit(const std::vector<uint8_t>& status, size_t bit)
{
    if (status.empty())
    {
        return false;
    }

    const size_t byteOffset = bit / 8;
    const size_t maxIdx = status.size() - 1;
    if (byteOffset > maxIdx)
    {
        return false;
    }

    return (status[maxIdx - byteOffset] >> (bit % 8)) & 1;
}

inline std::optional<uint8_t>
    getAPFatalErrorCode(const std::vector<uint8_t>& status)
{
    if (status.size() <= apFatalErrorCodeFirstBit / 8)
    {
        return std::nullopt;
    }

    uint8_t code = 0;
    for (size_t bit = 0; bit < apFatalErrorCodeBitCount; ++bit)
    {
        if (getBit(status, apFatalErrorCodeFirstBit + bit))
        {
            code |= static_cast<uint8_t>(1 << bit);
        }
    }

    return code;
}

inline bool isAPFatalErrorCodeNormal(const std::vector<uint8_t>& status)
{
    auto code = getAPFatalErrorCode(status);
    return code.has_value() && code.value() == 0;
}

inline bool isAPFatalErrorCodeSet(const std::vector<uint8_t>& status)
{
    auto code = getAPFatalErrorCode(status);
    return code.has_value() && code.value() != 0;
}

inline bool isAPBootComplete(const std::vector<uint8_t>& status)
{
    return getBit(status, ap0BootCompleteBit);
}

inline bool isAPBootCompleteTimeout(const std::vector<uint8_t>& status)
{
    return getBit(status, ap0BootCompleteTimeoutBit);
}

} // namespace nvidia::fw_status::boot_status
