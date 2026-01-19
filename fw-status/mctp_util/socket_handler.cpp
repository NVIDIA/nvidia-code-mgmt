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

#include <linux/mctp.h>
#include <sys/socket.h>

#include <phosphor-logging/lg2.hpp>

#include <iostream>

namespace mctp_socket
{

void Handler::processRxMsg(uint8_t eid, const std::vector<uint8_t>& requestMsg)
{
    auto msg = reinterpret_cast<const mctp_vdm::Message*>(requestMsg.data());
    if (msg->hdr.request == 0)
    {
        auto response = reinterpret_cast<const mctp_vdm::Message*>(msg);
        size_t responseLen =
            requestMsg.size() - sizeof(struct mctp_vdm::MsgHeader);
        handler.handleResponse(eid, msg->hdr.instanceId, msg->hdr.msgType,
                               msg->hdr.commandCode, response, responseLen);
    }
}

int Handler::initSocket([[maybe_unused]] int type,
                        [[maybe_unused]] int protocol,
                        [[maybe_unused]] const std::vector<uint8_t>& pathName)
{
    if (isFdValid)
    {
        return fd;
    }

    fd = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (fd == -1)
    {
        int rc = -errno;
        lg2::error("Failed to create MCTP socket, RC={RC}", "RC",
                   strerror(-rc));
        return rc;
    }

    socklen_t optlen = sizeof(sendBufferSize);
    int rc = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &optlen);
    if (rc == -1)
    {
        rc = -errno;
        lg2::error("Error getting socket send buffer size, RC={RC}", "RC",
                   strerror(-rc));
        close(fd);
        fd = -1;
        return rc;
    }

    struct sockaddr_mctp addr;
    memset(&addr, 0, sizeof(addr));

    addr.smctp_family = AF_MCTP;
    addr.smctp_network = MCTP_NET_ANY;
    addr.smctp_addr.s_addr = MCTP_ADDR_ANY;
    addr.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_type = mctp_vdm::MessageType;

    rc = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == -1)
    {
        rc = -errno;
        lg2::error("Error binding socket to MCTP VDM type, RC={RC}", "RC",
                   strerror(-rc));
        close(fd);
        fd = -1;
        return rc;
    }

    io = std::make_unique<IO>(
        event, fd, EPOLLIN, std::bind_front(&Handler::handleReceivedMsg, this));

    isFdValid = true;
    return fd;
}

void Handler::handleReceivedMsg(IO& io, int fd, uint32_t revents)
{
    if (!(revents & EPOLLIN))
    {
        return;
    }

    int returnCode = 0;
    ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
    if (peekedLength == 0)
    {
        lg2::error("Socket connection closed. Terminating.");
        io.get_event().exit(0);
        return;
    }
    else if (peekedLength < 0)
    {
        returnCode = -errno;
        lg2::error("recv system call failed, RC={RC}", "RC", returnCode);
        return;
    }

    std::vector<uint8_t> requestMsg(peekedLength);
    struct sockaddr_mctp addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addrlen = sizeof(addr);

    ssize_t recvDataLength =
        recvfrom(fd, static_cast<void*>(requestMsg.data()), peekedLength, 0,
                 reinterpret_cast<struct sockaddr*>(&addr), &addrlen);

    if (recvDataLength != peekedLength)
    {
        returnCode = -errno;
        lg2::error(
            "Failed to read complete packet. peekedLength={PEEKEDLENGTH} recvDataLength={RECVDATALENGTH} ErrorNo={ERROR}",
            "PEEKEDLENGTH", peekedLength, "RECVDATALENGTH", recvDataLength,
            "ERROR", returnCode);
        return;
    }

    if (addr.smctp_type != mctp_vdm::MessageType)
    {
        lg2::info("Skipping non-VDM message type: {TYPE}", "TYPE",
                  addr.smctp_type);
        return;
    }

    utils::printBuffer(utils::Rx, requestMsg);
    processRxMsg(addr.smctp_addr.s_addr, requestMsg);
}

} // namespace mctp_socket
