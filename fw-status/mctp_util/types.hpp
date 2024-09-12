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

#include <sdbusplus/message/types.hpp>

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dbus
{
using ObjectPath = std::string;
using Service = std::string;
using Interface = std::string;
using Interfaces = std::vector<std::string>;
using Property = std::string;
using PropertyType = std::string;
using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;

using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
using MapperServiceMap = std::vector<std::pair<Service, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;
} // namespace dbus

namespace mctp
{
using EID = uint8_t;
using UUID = std::string;
using Medium = std::string;
using NetworkId = uint8_t;

using Request = std::vector<uint8_t>;
using Response = std::vector<uint8_t>;
using Command = uint8_t;

using Info = std::tuple<EID, UUID, Medium, NetworkId>;
using Infos = std::vector<Info>;

using EidMap = std::unordered_map<EID, std::tuple<UUID, Medium>>;
} // namespace mctp

namespace mctp_vdm
{

/** @struct MsgHeader
 *
 * Structure representing MCTP VDM header fields
 */
struct MsgHeader
{
    uint32_t iana;
#if BYTE_ORDER == LITTLE_ENDIAN
    uint8_t instanceId : 5; //!< Instance ID
    uint8_t reserved : 1;   //!< Reserved
    uint8_t datagram : 1;   //!< Datagram bit
    uint8_t request : 1;    //!< Request bit
#endif
#if BYTE_ORDER == BIG_ENDIAN
    uint8_t request : 1;    //!< Request bit
    uint8_t datagram : 1;   //!< Datagram bit
    uint8_t reserved : 1;   //!< Reserved
    uint8_t instanceId : 5; //!< Instance ID
#endif
    uint8_t msgType;
    uint8_t commandCode;
    uint8_t msgVersion;
} __attribute__((packed));

/** @struct Message
 *
 * Structure representing MCTP VDM message
 */
struct Message
{
    struct MsgHeader hdr; //!< MCTP VDM message header
    uint8_t payload[1];   //!< &payload[0] is the beginning of the payload
} __attribute__((packed));

} // namespace mctp_vdm
