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

#include "handler.hpp"
#include "socket_manager.hpp"
#include "utils.hpp"

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <map>
#include <optional>
#include <unordered_map>

namespace mctp_socket
{

using PathName = std::string;
using namespace sdeventplus;
using namespace sdeventplus::source;

/** @class Handler
 *
 *  Base class for MCTP socket handlers that defines the interface for
 *  communication with MCTP endpoints.
 */
template <typename T = mctp_vdm::requester::RequestRetryTimer>
class Handler
{
  public:
    Handler() = delete;
    Handler(const Handler&) = delete;
    Handler(Handler&&) = default;
    Handler& operator=(const Handler&) = delete;
    Handler& operator=(Handler&&) = default;
    virtual ~Handler() = default;

    /** @brief Constructor
     *
     *  @param[in] event - daemon's main event loop
     *  @param[in] handler - MCTP VDM request handler
     *  @param[in/out] manager - MCTP socket manager
     */
    explicit Handler(sdeventplus::Event& event,
                     mctp_vdm::requester::Handler<T>& handler,
                     mctp_socket::Manager& manager) :
        event(event),
        handler(handler), manager(manager)
    {}

    /** @brief Register MCTP endpoint with socket information
     *
     *  @param[in] eid - MCTP endpoint ID
     *  @param[in] type - socket type
     *  @param[in] protocol - socket protocol
     *  @param[in] pathName - socket path name
     */
    void registerMctpEndpoint(uint8_t eid, int type, int protocol,
                              const std::vector<uint8_t>& pathName)
    {
        if (eidToSockMap.find(eid) == eidToSockMap.end())
        {
            eidToSockMap[eid] = std::make_tuple(type, protocol, pathName);
        }
    }

    /** @brief Activates sockets for the given EIDs
     *
     *  @param[in] eids - vector of MCTP endpoint IDs
     *  @return 0 on success, negative value on failure
     */
    int activateSockets(const std::vector<uint8_t>& eids)
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
                    lg2::error("Error initialising socket for EID={EID}", "EID",
                               eid);
                    continue;
                }
                else
                {
                    manager.registerEndpoint(eid, fd);
                }
            }
            else
            {
                manager.registerEndpoint(
                    eid, (*(std::get<0>(entry->second)).get())());
            }
        }
        return 0;
    }

    /** @brief Deactivates all sockets and clears endpoint registrations */
    void deactivateSockets()
    {
        socketInfoMap.clear();
        manager.clearMctpEndpoints();
    }

    /** @brief
     * Deactivates the socket handling communication with the EID, if no other
     * EIDs are communicating over the same socket
     *
     * @param eid - input EID to deactivate the socket for
     * */
    void deactivateSocket(uint8_t eid)
    {
        if (eidToSockMap.find(eid) == eidToSockMap.end())
        {
            return;
        }

        auto pathName = std::get<2>(eidToSockMap[eid]);
        auto entry = socketInfoMap.find(pathName);
        if (entry != socketInfoMap.end() && checkActiveEndpoints(eid))
        {
            socketInfoMap.erase(entry);
        }
        manager.clearMctpEndpoint(eid);
    }

  protected:
    sdeventplus::Event& event;
    mctp_vdm::requester::Handler<T>& handler;
    mctp_socket::Manager& manager;

    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<std::vector<uint8_t>,
             std::tuple<std::unique_ptr<utils::CustomFD>, std::unique_ptr<IO>>>
        socketInfoMap;

    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<uint8_t, std::tuple<int, int, std::vector<uint8_t>>> eidToSockMap;

    /** @brief
     * Checks for active requests on the same communication path as the input
     * EID
     *
     * @param eid - input EID to fetch the communication path
     * @return true if no other EIDs are using the same path
     * */
    bool checkActiveEndpoints(uint8_t eid)
    {
        auto activeEids = manager.getActiveEndpoints();
        auto currentEidPathName = std::get<2>(eidToSockMap[eid]);
        for (const uint8_t activeEid : activeEids)
        {
            if (activeEid == eid)
            {
                continue;
            }

            if (currentEidPathName == std::get<2>(eidToSockMap[activeEid]))
            {
                return false;
            }
        }
        return true;
    }

    /** @brief Initialize socket with given parameters
     *
     *  @param[in] type - socket type
     *  @param[in] protocol - socket protocol
     *  @param[in] pathName - socket path name
     *  @return socket file descriptor on success, negative value on failure
     */
    virtual int initSocket(int type, int protocol,
                           const std::vector<uint8_t>& pathName) = 0;

    /** @brief Process received MCTP message
     *
     *  @param[in] requestMsg - received message data
     */
    void processRxMsg(uint8_t eid, const std::vector<uint8_t>& requestMsg);
};

/** @class DaemonHandler
 *
 *  Handler implementation for MCTP communication via daemon
 */
class DaemonHandler : public Handler<mctp_vdm::requester::DaemonRequest>
{
  public:
    using Handler<mctp_vdm::requester::DaemonRequest>::Handler;

  private:
    int initSocket(int type, int protocol,
                   const std::vector<uint8_t>& pathName) override;
    void handleReceivedMsg(IO& io, int fd, uint32_t revents);
};

/** @class InKernelHandler
 *
 *  Handler implementation for in-kernel MCTP communication
 */
class InKernelHandler : public Handler<mctp_vdm::requester::InKernelRequest>
{
  public:
    using Handler<mctp_vdm::requester::InKernelRequest>::Handler;

  private:
    int initSocket(int type, int protocol,
                   const std::vector<uint8_t>& pathName) override;
    void handleReceivedMsg(IO& io, int fd, uint32_t revents);

    std::unique_ptr<IO> io;
    int fd{-1};
    int sendBufferSize{0};
    bool isFdValid{false};
};

} // namespace mctp_socket