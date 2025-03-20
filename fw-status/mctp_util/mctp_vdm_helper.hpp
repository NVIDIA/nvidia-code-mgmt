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

#include "instance_id.hpp"
#include "mctp_endpoint_discovery.hpp"
#include "types.hpp"

#include <coroutine>
#include <queue>
#include <tuple>
#include <unordered_map>

namespace mctp
{

using Priority = int;
static std::unordered_map<mctp::Medium, Priority> mediumPriority{
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 5}

};

struct MctpEidInfo
{
    uint8_t eid;
    mctp::Medium medium;

    friend bool operator<(MctpEidInfo const& lhs, MctpEidInfo const& rhs)
    {
        return mediumPriority.at(lhs.medium) > mediumPriority.at(rhs.medium);
    }
};

struct MCTPEidInfoPriorityQueue : std::priority_queue<MctpEidInfo>
{
    auto begin() const
    {
        return c.begin();
    }
    auto end() const
    {
        return c.end();
    }
};

using MctpInfoMap = std::unordered_map<UUID, MCTPEidInfoPriorityQueue>;

} // namespace mctp

class MctpDiscoveryHandlerIntf;

/** @class MCTPVdmHelper
 *
 *  ERoT time manager
 *  communicate with the endpoint. The lookup APIs are used when processing MCTP
 *  VDM Rx messages and when sending MCTP VDM Tx messages.
 */
template <typename T = mctp_vdm::requester::RequestRetryTimer>
class MCTPVdmHelper : public mctp_vdm::MctpDiscoveryHandlerIntf
{
  public:
    MCTPVdmHelper() = delete;
    MCTPVdmHelper(const MCTPVdmHelper&) = delete;
    MCTPVdmHelper(MCTPVdmHelper&&) = delete;
    MCTPVdmHelper& operator=(const MCTPVdmHelper&) = delete;
    MCTPVdmHelper& operator=(MCTPVdmHelper&&) = delete;
    ~MCTPVdmHelper() = default;

    /** @brief
     *
     *  @param[in] bus - reference to systemd bus
     *  @param[in] reqHandler - MCTP VDM requester handler
     *  @param[in] sockHandler - MCTP demux daemon socket handler
     *  @param[in] instanceIdMgr - Instance ID Manager
     */
    explicit MCTPVdmHelper(sdbusplus::bus::bus& bus,
                           mctp_vdm::requester::Handler<T>& reqHandler,
                           mctp_socket::Handler<T>& sockHandler,
                           mctp_vdm::InstanceIdMgr& instanceIdMgr);

    mctp_vdm::requester::Coroutine
        queryBootStatus(uint8_t eid, const mctp_vdm::Message*& responseMsg,
                        size_t& responseLen);
    mctp_vdm::requester::Coroutine
        queryBootStatusImpl(uint8_t eid, const mctp_vdm::Message*& responseMsg,
                            size_t& responseLen);

    mctp_vdm::requester::Coroutine handleMctpEndpointsTask();

    void handleMctpEndpoints([[maybe_unused]] const mctp::Infos& mctpInfos);

  private:
    /** @brief reference to the systemd bus */
    sdbusplus::bus::bus& bus;

    mctp_vdm::requester::Handler<T>& reqHandler;

    mctp_socket::Handler<T>& sockHandler;

    mctp::MctpInfoMap mctpInfoMap;

    mctp_vdm::InstanceIdMgr& instanceIdMgr;

    /** @brief A queue of MctpInfos to be discovered **/
    std::queue<mctp::Infos> queuedMctpInfos{};
};
