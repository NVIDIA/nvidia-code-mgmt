/* 
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved. 
 * SPDX-License-Identifier: Apache-2.0 
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
#include "mctp_vdm_completion_codes.hpp"
#include "request.hpp"
#include "socket_manager.hpp"
#include "types.hpp"

#include <function2/function2.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>

#include <cassert>
#include <chrono>
#include <coroutine>
#include <iostream>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace mctp_vdm
{

namespace requester
{

/** @struct RequestKey
 *
 *  RequestKey uniquely identifies the MCTP VDM request message to match it with
 *  the response and a combination of MCTP endpoint ID, MCTP VDM instance ID,
 *  MCTP VDM type and MCTP VDM command is the key.
 */
struct RequestKey
{
    uint8_t eid;        //!< MCTP endpoint ID
    uint8_t instanceId; //!< MCTP VDM instance ID
    uint8_t type;       //!< MCTP VDM type
    uint8_t command;    //!< MCTP VDM command

    bool operator==(const RequestKey& e) const
    {
        return ((eid == e.eid) && (instanceId == e.instanceId) &&
                (type == e.type) && (command == e.command));
    }
};

/** @struct RequestKeyHasher
 *
 *  This is a simple hash function, since the instance ID generator API
 *  generates unique instance IDs for MCTP endpoint ID.
 */
struct RequestKeyHasher
{
    std::size_t operator()(const RequestKey& key) const
    {
        return (key.eid << 24 | key.instanceId << 16 | key.type << 8 |
                key.command);
    }
};

using ResponseHandler = fu2::unique_function<void(
    uint8_t eid, const Message* response, size_t respMsgLen)>;

/** @class Handler
 *
 *  This class handles the lifecycle of the MCTP VDM request message based on
 *  the instance ID expiration interval, number of request retries and the
 *  timeout waiting for a response. The registered response handlers are invoked
 *  with response once the MCTP VDM responder sends the response. If no response
 *  is received within the instance ID expiration interval or any other failure
 *  the response handler is invoked with the empty response.
 *
 * @tparam RequestInterface - Request class type
 */
template <class RequestInterface>
class Handler
{

  public:
    Handler() = delete;
    Handler(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler& operator=(Handler&&) = delete;
    ~Handler() = default;

    /** @brief Constructor
     *
     *  @param[in] event - reference to daemon's main event loop
     *  @param[in] requester - reference to Requester object
     *  @param[in] sockManager - MCTP socket manager
     *  @param[in] instanceIdExpiryInterval - instance ID expiration interval
     *  @param[in] numRetries - number of request retries
     *  @param[in] responseTimeOut - time to wait between each retry
     */
    explicit Handler(sdeventplus::Event& event,
                     mctp_vdm::InstanceIdMgr& instanceIdMgr,
                     mctp_socket::Manager& sockManager,
                     std::chrono::seconds instanceIdExpiryInterval =
                         std::chrono::seconds(15),
                     uint8_t numRetries = numCommandRetries,
                     std::chrono::milliseconds responseTimeOut =
                         std::chrono::milliseconds(5000)) :
        event(event),
        instanceIdMgr(instanceIdMgr), sockManager(sockManager),
        instanceIdExpiryInterval(instanceIdExpiryInterval),
        numRetries(numRetries), responseTimeOut(responseTimeOut)
    {}

    /** @brief Register a MCTP VDM request message
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] instanceId - instance ID to match request and response
     *  @param[in] type - MCTP VDM Message type
     *  @param[in] command - MCTP VDM command
     *  @param[in] requestMsg - MCTP VDM request message
     *  @param[in] responseHandler - Response handler for this request
     *
     *  @return return MCTP_VDM_SUCCESS on success and MCTP_VDM_ERROR otherwise
     */
    int registerRequest(uint8_t eid, uint8_t instanceId, uint8_t type,
                        uint8_t command, mctp::Request&& requestMsg,
                        ResponseHandler&& responseHandler)
    {
        RequestKey key{eid, instanceId, type, command};

        auto instanceIdExpiryCallBack = [key, this](void) {
            if (this->handlers.contains(key))
            {
                lg2::error("Response not received for the request, instance ID "
                           "expired. EID={EID}, INSTANCE_ID={INSTANCE_ID} ,"
                           "TYPE={TYPE}, COMMAND={COMMAND}",
                           "EID", key.eid, "INSTANCE_ID", key.instanceId,
                           "TYPE", key.type, "COMMAND", key.command);
                auto& [request, responseHandler, timerInstance] =
                    this->handlers[key];
                request->stop();
                auto rc = timerInstance->stop();
                if (rc)
                {
                    lg2::error(
                        "Failed to stop the instance ID expiry timer. RC={RC}",
                        "RC", rc);
                }
                // Call response handler with an empty response to indicate no
                // response
                responseHandler(key.eid, nullptr, 0);
                this->removeRequestContainer.emplace(
                    key, std::make_unique<sdeventplus::source::Defer>(
                             event, std::bind(&Handler::removeRequestEntry,
                                              this, key)));
            }
            else
            {
                // This condition is not possible, if a response is received
                // before the instance ID expiry, then the response handler
                // is executed and the entry will be removed.
                assert(false);
            }
        };

        auto request = std::make_unique<RequestInterface>(
            sockManager.getSocket(eid), eid, event, std::move(requestMsg),
            numRetries, responseTimeOut);
        auto timer = std::make_unique<sdbusplus::Timer>(
            event.get(), instanceIdExpiryCallBack);

        handlers.emplace(key, std::make_tuple(std::move(request),
                                              std::move(responseHandler),
                                              std::move(timer)));
        return runRegisteredRequest(eid);
    }

    int runRegisteredRequest(uint8_t eid)
    {
        RequestValue* toRun = nullptr;
        uint8_t instanceId = 0;
        for (auto& handler : handlers)
        {
            auto& key = handler.first;
            auto& [request, responseHandler, timerInstance] = handler.second;
            if (key.eid != eid)
            {
                continue;
            }

            if (timerInstance->isRunning())
            {
                // A MCTP VDM request for the EID is running
                return static_cast<int>(mctp_vdm::CompletionCodes::Success);
            }

            if (toRun == nullptr)
            {
                // First request of the EID
                toRun = &handler.second;
                instanceId = key.instanceId;
            }
        }

        if (toRun != nullptr)
        {
            auto& [request, responseHandler, timerInstance] = *toRun;
            auto rc = request->start();
            if (rc)
            {
                instanceIdMgr.markFree(eid, instanceId);
                lg2::error("Failure to send the MCTP VDM request message");
                return rc;
            }

            try
            {
                timerInstance->start(duration_cast<std::chrono::microseconds>(
                    instanceIdExpiryInterval));
            }
            catch (const std::runtime_error& e)
            {
                instanceIdMgr.markFree(eid, instanceId);
                lg2::error("Failed to start the instance ID expiry timer.",
                           "ERROR", e);
                return static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral);
            }
        }
        return static_cast<int>(mctp_vdm::CompletionCodes::Success);
    }

    /** @brief Handle MCTP VDM response message
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] instanceId - instance ID to match request and response
     *  @param[in] type - MCTP VDM type
     *  @param[in] command - MCTP VDM command
     *  @param[in] response - MCTP VDM response message
     *  @param[in] respMsgLen - length of the response message
     */
    void handleResponse(uint8_t eid, uint8_t instanceId, uint8_t type,
                        uint8_t command, const mctp_vdm::Message* response,
                        size_t respMsgLen)
    {
        RequestKey key{eid, instanceId, type, command};
        if (handlers.contains(key))
        {
            auto& [request, responseHandler, timerInstance] = handlers[key];
            request->stop();
            auto rc = timerInstance->stop();
            if (rc)
            {
                lg2::error(
                    "Failed to stop the instance ID expiry timer. RC={RC}",
                    "RC", rc);
            }
            // Call responseHandler after erase it from the handlers to avoid
            // starting it again in runRegisteredRequest()
            auto unique_handler = std::move(responseHandler);
            instanceIdMgr.markFree(key.eid, key.instanceId);
            handlers.erase(key);
            unique_handler(eid, response, respMsgLen);
        }
        else
        {
            // Got a response for a MCTP VDM request message not registered with
            // the request handler, this can be from other mctp-vdm-util
            // instances.
            instanceIdMgr.markFree(key.eid, key.instanceId);
        }
        runRegisteredRequest(eid);
    }

  private:
    int fd; //!< file descriptor of MCTP communications socket
    sdeventplus::Event& event; //!< reference to daemon's main event loop
    mctp_vdm::InstanceIdMgr& instanceIdMgr; //!< reference to Requester object
    mctp_socket::Manager& sockManager;

    std::chrono::seconds
        instanceIdExpiryInterval; //!< Instance ID expiration interval
    uint8_t numRetries;           //!< number of request retries
    std::chrono::milliseconds
        responseTimeOut; //!< time to wait between each retry

    /** @brief Container for storing the details of the MCTP VDM request
     *         message, handler for the corresponding MCTP VDM response and the
     *         timer object for the Instance ID expiration
     */
    using RequestValue =
        std::tuple<std::unique_ptr<RequestInterface>, ResponseHandler,
                   std::unique_ptr<sdbusplus::Timer>>;

    /** @brief Container for storing the MCTP VDM request entries */
    std::unordered_map<RequestKey, RequestValue, RequestKeyHasher> handlers;

    /** @brief Container to store information about the request entries to be
     *         removed after the instance ID timer expires
     */
    std::unordered_map<RequestKey, std::unique_ptr<sdeventplus::source::Defer>,
                       RequestKeyHasher>
        removeRequestContainer;

    /** @brief Remove request entry for which the instance ID expired
     *
     *  @param[in] key - key for the Request
     */
    void removeRequestEntry(RequestKey key)
    {
        if (removeRequestContainer.contains(key))
        {
            removeRequestContainer[key].reset();
            instanceIdMgr.markFree(key.eid, key.instanceId);
            handlers.erase(key);
            removeRequestContainer.erase(key);
        }
        runRegisteredRequest(key.eid);
    }
};

/** @struct SendRecvMctpVdmMsg
 *
 * An awaitable object needed by co_await operator to send/recv MCTP VDM message
 *
 * e.g.
 * rc = co_await SendRecvMctpVdmMsg<h>(h, eid, req, respMsg, respLen);
 *
 * @tparam RequesterHandler - Requester::handler class type
 */
template <class RequesterHandler>
struct SendRecvMctpVdmMsg
{
    /** @brief For recording the suspended coroutine where the co_await
     * operator is. When MCTP VDM response message is received, the
     * resumeHandle() will be called to continue the next line of co_await
     * operator
     */
    std::coroutine_handle<> resumeHandle;

    /** @brief The RequesterHandler to send/recv MCTP VDM message.
     */
    RequesterHandler& handler;

    /** @brief The EID where MCTP VDM message will be sent to.
     */
    uint8_t eid;

    /** @brief The MCTP VDM request message.
     */
    mctp::Request& request;

    /** @brief The pointer of MCTP VDM response message.
     */
    const mctp_vdm::Message** responseMsg;

    /** @brief The length of MCTP VDM response message.
     */
    size_t* responseLen;

    /** @brief For keeping the return value of RequesterHandler.
     */
    uint8_t rc;

    /** @brief Returning false to make await_suspend() to be called.
     */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending coroutine. The
     * method will send out MCTP VDM request message, register handleResponse()
     * as call back function for the event when MCTP VDM response message
     * received.
     */
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        if (responseMsg == nullptr || responseLen == nullptr)
        {
            rc = static_cast<int>(mctp_vdm::CompletionCodes::ErrInvalidData);
            return false;
        }

        auto requestMsg = reinterpret_cast<mctp_vdm::Message*>(request.data());
        rc = handler.registerRequest(
            eid, requestMsg->hdr.instanceId, requestMsg->hdr.msgType,
            requestMsg->hdr.commandCode, std::move(request),
            std::move(
                std::bind_front(&SendRecvMctpVdmMsg::HandleResponse, this)));

        lg2::error("Register Request successful");
        if (rc)
        {
            lg2::error("registerRequest failed, rc={RC}", "RC",
                       static_cast<unsigned>(rc));
            return false;
        }

        resumeHandle = handle;
        return true;
    }

    /** @brief Called by co_await operator to get return value when awaitable
     * object completed.
     */
    uint8_t await_resume() const noexcept
    {
        return rc;
    }

    /** @brief Constructor of awaitable object to initialize necessary member
     * variables.
     */
    SendRecvMctpVdmMsg(RequesterHandler& handler, uint8_t eid,
                       mctp::Request& request,
                       const mctp_vdm::Message** responseMsg,
                       size_t* responseLen) :
        handler(handler),
        eid(eid), request(request), responseMsg(responseMsg),
        responseLen(responseLen),
        rc(static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral))
    {}

    /** @brief The function will be registered by ReqisterHandler for handling
     * MCTP VDM response message. The copied responseMsg is for preventing that
     * the response pointer in parameter becomes invalid when coroutine is
     * resumed.
     */
    void HandleResponse(uint8_t eid, const mctp_vdm::Message* response,
                        size_t length)
    {
        lg2::error("Handle response called");
        if (response == nullptr || !length)
        {
            lg2::error("No response received, EID={EID}", "EID", eid);
            rc = static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral);
        }
        else
        {
            *responseMsg = response;
            *responseLen = length;
            rc = static_cast<int>(mctp_vdm::CompletionCodes::Success);
        }
        resumeHandle();
    }
};

/** @struct Coroutine
 *
 * A coroutine return_object supports nesting coroutine
 */
struct Coroutine
{
    /** @brief The nested struct named 'promise_type' which is needed for
     * Coroutine struct to be a coroutine return_object.
     */
    struct promise_type
    {
        /** @brief For keeping the parent coroutine handle if any. For the case
         * of nesting co_await coroutine, this handle will be used to resume to
         * continue parent coroutine.
         */
        std::coroutine_handle<> parent_handle;

        /** @brief For holding return value of coroutine
         */
        uint8_t data;

        bool detached = false;

        /** @brief Get the return object object
         */
        Coroutine get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /** @brief The method is called before starting a coroutine. Returning
         * std::suspend_never awaitable to execute coroutine body immediately.
         */
        std::suspend_never initial_suspend()
        {
            return {};
        }

        /** @brief The method is called after coroutine completed to return a
         * customized awaitable object to resume to parent coroutine if any.
         */
        auto final_suspend() noexcept
        {
            struct awaiter
            {
                /** @brief Returning false to make await_suspend to be called.
                 */
                bool await_ready() const noexcept
                {
                    return false;
                }

                /** @brief Do nothing here for customized awaitable object.
                 */
                void await_resume() const noexcept
                {}

                /** @brief Returning parent coroutine handle here to continue
                 * parent corotuine.
                 */
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept
                {
                    auto parent_handle = h.promise().parent_handle;
                    if (h.promise().detached)
                    {
                        h.destroy();
                    }
                    if (parent_handle)
                    {
                        return parent_handle;
                    }
                    return std::noop_coroutine();
                }
            };
            return awaiter{};
        }

        /** @brief The handler for an exception was thrown in
         * coroutine body.
         */
        void unhandled_exception()
        {}

        /** @brief Keeping the value returned by co_return operator
         */
        void return_value(uint8_t value) noexcept
        {
            data = std::move(value);
        }
    };

    /** @brief Called by co_await to check if it needs to be
     * suspened.
     */
    bool await_ready() const noexcept
    {
        return handle.done();
    }

    /** @brief Called by co_await operator to get return value when coroutine
     * finished.
     */
    uint8_t await_resume() const noexcept
    {
        return std::move(handle.promise().data);
    }

    /** @brief Called when the coroutine itself is being suspended. The
     * recording the parent coroutine handle is for await_suspend() in
     * promise_type::final_suspend to refer.
     */
    bool await_suspend(std::coroutine_handle<> coroutine)
    {
        handle.promise().parent_handle = coroutine;
        return true;
    }

    ~Coroutine()
    {
        if (handle && handle.done())
        {
            handle.destroy();
        }
    }

    void detach()
    {
        if (!handle)
        {
            return;
        }

        if (handle.done())
        {
            handle.destroy();
        }
        else
        {
            handle.promise().detached = true;
        }
        handle = nullptr;
    }

    /** @brief Assigned by promise_type::get_return_object to keep coroutine
     * handle itself.
     */
    mutable std::coroutine_handle<promise_type> handle;
};

} // namespace requester

} // namespace mctp_vdm
