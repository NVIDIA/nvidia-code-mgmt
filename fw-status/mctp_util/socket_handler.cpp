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

#include "socket_handler.hpp"

#include "constants.hpp"
#include "utils.hpp"

#include <libmctp-externals.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <phosphor-logging/lg2.hpp>

#include <iostream>

namespace mctp_socket
{

void Handler::registerMctpEndpoint(uint8_t eid, int type, int protocol,
                                   const std::vector<uint8_t>& pathName)
{

    if (!eidToSockMap.contains(eid))
    {
        eidToSockMap[eid] = std::make_tuple(type, protocol, pathName);
    }
}

int Handler::activateSockets(const std::vector<uint8_t>& eids)
{
    for (const auto& eid : eids)
    {
        auto type = std::get<0>(eidToSockMap[eid]);
        auto protocol = std::get<1>(eidToSockMap[eid]);
        auto pathName = std::get<2>(eidToSockMap[eid]);

        auto entry = socketInfoMap.find(pathName);
        if (entry == socketInfoMap.end())
        {
            auto fd = initSocket(type, protocol, pathName);
            if (fd < 0)
            {
                continue;
            }
            else
            {
                manager.registerEndpoint(eid, fd);
            }
        }
        else
        {
            manager.registerEndpoint(eid,
                                     (*(std::get<0>(entry->second)).get())());
        }
    }
    return 0;
}

void Handler::deactivateSockets()
{
    socketInfoMap.clear();
    manager.clearMctpEndpoints();
}

int Handler::initSocket(int type, int protocol,
                        const std::vector<uint8_t>& pathName)
{
    auto callback = [this](IO& io, int fd, uint32_t revents) mutable {
        if (!(revents & EPOLLIN))
        {
            return;
        }

        int returnCode = 0;
        ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
        if (peekedLength == 0)
        {
            // MCTP daemon has closed the socket this daemon is connected to.
            // This may or may not be an error scenario, in either case the
            // recovery mechanism for this daemon is to restart, and hence
            // exit the event loop, that will cause this daemon to exit with a
            // failure code.
            io.get_event().exit(0);
        }
        else if (peekedLength <= -1)
        {
            returnCode = -errno;
            lg2::error("recv system call failed, RC={RC}", "RC", returnCode);
        }
        else
        {
            std::vector<uint8_t> requestMsg(peekedLength);
            auto recvDataLength = recv(
                fd, static_cast<void*>(requestMsg.data()), peekedLength, 0);
            if (recvDataLength == peekedLength)
            {
                utils::printBuffer(utils::Rx, requestMsg);

                if (mctp_vdm::MessageType != requestMsg[2])
                {
                    // Skip this message and continue.
                }
                else
                {
                    processRxMsg(requestMsg);
                }
            }
            else
            {
                lg2::error("Failure to read peeked length packet. peekedLength="
                           "{PEEKEDLENGTH} recvDataLength={RECVDATALENGTH}",
                           "PEEKEDLENGTH", peekedLength, "RECVDATALENGTH",
                           recvDataLength);
            }
        }
    };

    /* Create socket */
    int rc = 0;
    int sockFd = socket(AF_UNIX, type, protocol);
    if (sockFd == -1)
    {
        rc = -errno;
        lg2::error("Failed to create the socket, RC={RC}", "RC", strerror(-rc));
        return rc;
    }

    auto fd = std::make_unique<utils::CustomFD>(sockFd);

    // /* Initiate a connection to the socket */
    struct sockaddr_un addr
    {};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, pathName.data(), pathName.size());
    rc = connect(sockFd, reinterpret_cast<struct sockaddr*>(&addr),
                 pathName.size() + sizeof(addr.sun_family));
    if (rc == -1)
    {
        rc = -errno;
        lg2::error("Failed to connect to the socket, RC={RC}", "RC",
                   strerror(-rc));
        return rc;
    }

    /* Register for MCTP VDM message type */
    ssize_t result =
        write(sockFd, &mctp_vdm::MessageType, sizeof(mctp_vdm::MessageType));
    if (result == -1)
    {
        rc = -errno;
        lg2::error(
            "Failed to send message type as MCTP VDM to demux daemon, RC={RC}",
            "RC", strerror(-rc));
        return rc;
    }

    auto io = std::make_unique<IO>(event, sockFd, EPOLLIN, std::move(callback));
    socketInfoMap[pathName] = std::tuple(std::move(fd), std::move(io));

    return sockFd;
}

void Handler::processRxMsg(const std::vector<uint8_t>& requestMsg)
{
    using type = uint8_t;
    using tag_owner_and_tag = uint8_t;
    uint8_t eid = requestMsg[1];
    auto msg = reinterpret_cast<const mctp_vdm::Message*>(
        requestMsg.data() + sizeof(tag_owner_and_tag) + sizeof(eid) +
        sizeof(type));

    if (msg->hdr.request == 0)
    {
        auto response = reinterpret_cast<const mctp_vdm::Message*>(msg);
        size_t responseLen = requestMsg.size() -
                             sizeof(struct mctp_vdm::MsgHeader) - sizeof(eid) -
                             sizeof(type) - sizeof(tag_owner_and_tag);
        handler.handleResponse(eid, msg->hdr.instanceId, msg->hdr.msgType,
                               msg->hdr.commandCode, response, responseLen);
    }
}

} // namespace mctp_socket