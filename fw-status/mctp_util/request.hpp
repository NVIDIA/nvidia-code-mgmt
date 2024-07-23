#pragma once

#include "constants.hpp"
#include "mctp_vdm_completion_codes.hpp"
#include "types.hpp"
#include "utils.hpp"

#include <libmctp-externals.h>
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
        timeout;           //!< time to wait between each retry in milliseconds
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

/** @class Request
 *
 *  The concrete implementation of RequestIntf. This class implements the send()
 *  to send the MCTP VDM request message over MCTP socket.
 *  This class encapsulates the MCTP VDM request message, the number of times
 *  the request needs to retried if the response is not received and the amount
 *  of time to wait between each retry. It provides APIs to start and stop the
 *  request flow.
 */
class Request final : public RequestRetryTimer
{
  public:
    Request() = delete;
    Request(const Request&) = delete;
    Request(Request&&) = default;
    Request& operator=(const Request&) = delete;
    Request& operator=(Request&&) = default;
    ~Request() = default;

    /** @brief Constructor
     *
     *  @param[in] fd - fd of the MCTP communication socket
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] event - reference to daemon's main event loop
     *  @param[in] requestMsg - MCTP VDM request message
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit Request(int fd, uint8_t eid, sdeventplus::Event& event,
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

        utils::printBuffer(utils::Tx, requestMsg);

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
                "Failed to send MCTP VDM message. RC={RC}, errno={ERRNO}", "RC",
                unsigned(rc), "ERRNO", strerror(errno));
            return returnCode;
        }
        return returnCode;
    }
};

} // namespace requester

} // namespace mctp_vdm
