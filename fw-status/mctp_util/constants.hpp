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

#include "stddef.h"
#include "stdint.h"

#include <string_view>

namespace mapper
{
constexpr auto Service = "xyz.openbmc_project.ObjectMapper";
constexpr auto Path = "/xyz/openbmc_project/object_mapper";
constexpr auto Interface = "xyz.openbmc_project.ObjectMapper";
} // namespace mapper

namespace mctp
{
constexpr auto UUIDInterface{"xyz.openbmc_project.Common.UUID"};
}

namespace mctp_vdm
{
// Message Type for Vendor defined - IANA
constexpr uint8_t MessageType = 0x7F;

// MCTP VDM header
constexpr uint32_t nvidiaIANA = 5703;
constexpr uint8_t nvidiaMsgType = 1;
constexpr uint8_t nvidiaMsgVersion = 1;

/* Add external timestamp*/
constexpr uint8_t addExtTimestamp = 0x13;
constexpr size_t addExtTimestampReqBytes = 8;
constexpr size_t addExtTimestampRespBytes = 1;

constexpr uint8_t numCommandRetries = 3;
} // namespace mctp_vdm

namespace pldm
{
constexpr auto Service = "xyz.openbmc_project.PLDM";
constexpr auto Path = "/";
} // namespace pldm
