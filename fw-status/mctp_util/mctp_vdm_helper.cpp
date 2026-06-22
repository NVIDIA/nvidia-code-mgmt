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
#include "config.h"

#include "mctp_vdm_helper.hpp"

#include "mctp_vdm_completion_codes.hpp"
#include "types.hpp"
#include "utils.hpp"

#include <xyz/openbmc_project/Logging/Entry/server.hpp>

using namespace mctp_vdm;

MCTPVdmHelper::MCTPVdmHelper(sdbusplus::bus_t& bus,
                             mctp_vdm::requester::Handler& reqHandler,
                             mctp_socket::Handler& sockHandler,
                             mctp_vdm::InstanceIdMgr& instanceIdMgr) :
    bus(bus), reqHandler(reqHandler), sockHandler(sockHandler),
    instanceIdMgr(instanceIdMgr)
{}

mctp_vdm::requester::Coroutine MCTPVdmHelper::queryBootStatus(
    uint8_t eid, const mctp_vdm::Message*& responseMsg, size_t& responseLen)
{
    // Initialize MCTP sockets for the list of endpoints
    auto ret = sockHandler.activateSockets({eid});
    if (ret < 0)
    {
        lg2::error("Activating MCTP sockets failed. ret={RET}", "RET",
                   unsigned(ret));
        co_return ret;
    }

    auto rc = co_await queryBootStatusImpl(eid, responseMsg, responseLen);
    if (rc != 0 or responseMsg == nullptr)
    {
        lg2::error(
            "Fetching QueryBootStatus on ERoT failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
    }

    sockHandler.deactivateSocket(eid);

    co_return rc;
}

mctp_vdm::requester::Coroutine MCTPVdmHelper::queryBootStatusImpl(
    uint8_t eid, const mctp_vdm::Message*& responseMsg, size_t& responseLen)
{
    mctp::Request request(sizeof(mctp_vdm::MsgHeader));
    auto requestMsg = reinterpret_cast<mctp_vdm::MsgHeader*>(request.data());
    requestMsg->iana = htobe32(nvidiaIANA);
    requestMsg->request = 1;
    requestMsg->instanceId = instanceIdMgr.getInstanceId(eid);
    requestMsg->msgType = nvidiaMsgType;
    requestMsg->commandCode = 0x05;
    requestMsg->msgVersion = nvidiaMsgVersion;

    auto rc = co_await mctp_vdm::requester::SendRecvMctpVdmMsg(
        reqHandler, eid, request, &responseMsg, &responseLen);
    if (rc)
    {
        co_return rc;
    }

    if (responseMsg == nullptr)
    {
        co_return -1;
    }

    co_return responseMsg->payload[0];
}

void MCTPVdmHelper::handleMctpEndpoints(
    [[maybe_unused]] const mctp::Infos& mctpInfos)
{
    return;
}
