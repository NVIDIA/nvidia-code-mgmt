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

#include "constants.hpp"
#include "mctp_vdm_completion_codes.hpp"
#include "types.hpp"
#include "utils.hpp"

#include <libmctp-externals.h>
#include <linux/if_arp.h>
#include <linux/mctp.h>
#include <sys/socket.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <chrono>
#include <functional>

namespace mctp_vdm
{

namespace requester
{

/** @class RequestRetryTimer
 *
 *  The abstract base class for implementing the MCTP VDM request retry logic.
 *  This class handles number of times the MCTP VDM request needs to be retried
 *  if the response is not received and the time to wait between each retry. It
 *  provides APIs to start and stop the request flow.
 */
class RequestRetryTimer
{
  public:
    RequestRetryTimer() = delete;
    RequestRetryTimer(const RequestRetryTimer&) = delete;
    RequestRetryTimer(RequestRetryTimer&&) = default;
    RequestRetryTimer& operator=(const RequestRetryTimer&) = delete;
    RequestRetryTimer& operator=(RequestRetryTimer&&) = default;
    virtual ~RequestRetryTimer() = default;

    /** @brief Constructor
     *
     *  @param[in] event - reference to daemon's main event loop
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit RequestRetryTimer(sdeventplus::Event& event, uint8_t numRetries,
                               std::chrono::milliseconds timeout) :

        event(event),
        numRetries(numRetries), timeout(timeout),
        timer(event.get(), std::bind_front(&RequestRetryTimer::callback, this))
    {}

    /** @brief Starts the request flow and arms the timer for request retries
     *
     *  @return return 0 on success and -errno on failure
     */
    int start()
    {
        auto rc = send();
        if (rc)
        {
            return rc;
        }

        try
        {
            if (numRetries)
            {
                timer.start(duration_cast<std::chrono::microseconds>(timeout),
                            true);
            }
        }
        catch (const std::runtime_error& e)
        {
            lg2::error("Failed to start the request timer.", "ERROR", e);
            return -1;
        }

        return 0;
    }

    /** @brief Stops the timer and no further request retries happen */
    void stop()
    {
        auto rc = timer.stop();
        if (rc)
        {
            lg2::error("Failed to stop the request timer. RC={RC}", "RC",
                       unsigned(rc));
        }
    }

  protected:
    sdeventplus::Event& event; //!< reference to daemon's main event loop
    uint8_t numRetries;        //!< number of request retries
    std::chrono::milliseconds
        timeout;            //!< time to wait between each retry in milliseconds
    sdbusplus::Timer timer; //!< manages starting timers and handling timeouts

    /** @brief Sends the MCTP VDM request message
     *
     *  @return return MCTP_VDM_SUCCESS on success and MCTP_VDM_ERROR otherwise
     */
    virtual int send() const = 0;

    /** @brief Callback function invoked when the timeout happens */
    void callback()
    {
        if (numRetries--)
        {
            send();
        }
        else
        {
            stop();
        }
    }
};

/** @class DaemonRequest
 *
 *  The concrete implementation of RequestIntf. This class implements the send()
 *  to send the MCTP VDM request message over MCTP socket.
 *  This class encapsulates the MCTP VDM request message, the number of times
 *  the request needs to retried if the response is not received and the amount
 *  of time to wait between each retry. It provides APIs to start and stop the
 *  request flow.
 */
class DaemonRequest final : public RequestRetryTimer
{
  public:
    DaemonRequest() = delete;
    DaemonRequest(const DaemonRequest&) = delete;
    DaemonRequest(DaemonRequest&&) = default;
    DaemonRequest& operator=(const DaemonRequest&) = delete;
    DaemonRequest& operator=(DaemonRequest&&) = default;
    ~DaemonRequest() = default;

    /** @brief Constructor
     *
     *  @param[in] fd - fd of the MCTP communication socket
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] event - reference to daemon's main event loop
     *  @param[in] requestMsg - MCTP VDM request message
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit DaemonRequest(int fd, uint8_t eid, sdeventplus::Event& event,
                           mctp::Request&& requestMsg, uint8_t numRetries,
                           std::chrono::milliseconds timeout) :
        RequestRetryTimer(event, numRetries, timeout),
        fd(fd), eid(eid), requestMsg(std::move(requestMsg))
    {}

  private:
    int fd;                   //!< file descriptor of MCTP communications socket
    uint8_t eid;              //!< endpoint ID of the remote MCTP endpoint
    mctp::Request requestMsg; //!< MCTP VDM request message

    /** @brief Sends the MCTP VDM request message on the socket
     *
     *  @return return  0 on success and -errno on failure
     */
    int send() const
    {

        utils::printBuffer(utils::Tx, requestMsg, eid);

        uint8_t hdr[3] = {LIBMCTP_TAG_OWNER_MASK | MCTP_TAG_VDM, eid,
                          mctp_vdm::MessageType};

        struct iovec iov[2];
        iov[0].iov_base = hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = (uint8_t*)requestMsg.data();
        iov[1].iov_len = requestMsg.size();

        struct msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = sizeof(iov) / sizeof(iov[0]);

        int returnCode = 0;
        ssize_t rc = sendmsg(fd, &msg, 0);
        if (rc < 0)
        {
            int returnCode = -errno;
            lg2::error(
                "Failed to send MCTP VDM message. EID={EID}, RC={RC}, errno={ERRNO}",
                "EID", eid, "RC", unsigned(rc), "ERRNO", strerror(errno));
            return returnCode;
        }
        return returnCode;
    }
};

/** @class InKernelRequest
 *
 *  Class for handling in-kernel MCTP requests
 */
class InKernelRequest final : public RequestRetryTimer
{
  public:
    InKernelRequest() = delete;
    InKernelRequest(const InKernelRequest&) = delete;
    InKernelRequest(InKernelRequest&&) = default;
    InKernelRequest& operator=(const InKernelRequest&) = delete;
    InKernelRequest& operator=(InKernelRequest&&) = default;
    ~InKernelRequest() = default;

    // using RequestRetryTimer::RequestRetryTimer;

    /** @brief Constructor
     *
     *  @param[in] fd - fd of the MCTP communication socket
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] event - reference to daemon's main event loop
     *  @param[in] requestMsg - MCTP VDM request message
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit InKernelRequest(int fd, uint8_t eid, sdeventplus::Event& event,
                             mctp::Request&& requestMsg, uint8_t numRetries,
                             std::chrono::milliseconds timeout) :
        RequestRetryTimer(event, numRetries, timeout),
        fd(fd), eid(eid), requestMsg(std::move(requestMsg))
    {}

  private:
    int fd;                   //!< file descriptor of MCTP communications socket
    uint8_t eid;              //!< endpoint ID of the remote MCTP endpoint
    mctp::Request requestMsg; //!< MCTP VDM request message

    // const uint8_t MCTP_MSG_TYPE_PLDM = 1;

    /** @brief Send the request message
     *
     *  @return 0 on success, negative value on failure
     */
    int send() const
    {
        int returnCode{0};

        utils::printBuffer(utils::Tx, requestMsg, eid);

        struct sockaddr_mctp destAddr = {}; // Initialize at declaration
        destAddr.smctp_family = AF_MCTP;
        destAddr.smctp_network = MCTP_NET_ANY;
        destAddr.smctp_addr.s_addr = eid;
        destAddr.smctp_tag = MCTP_TAG_OWNER;
        destAddr.smctp_type = mctp_vdm::MessageType;

        ssize_t rc = sendto(fd, requestMsg.data(), requestMsg.size(), 0,
                            reinterpret_cast<struct sockaddr*>(&destAddr),
                            sizeof(destAddr));
        if (rc == -1)
        {
            returnCode = -errno;
            lg2::error("sendmsg system call failed, RC={RC}", "RC", returnCode);
        }

        return returnCode;
    }
};

} // namespace requester

} // namespace mctp_vdm
