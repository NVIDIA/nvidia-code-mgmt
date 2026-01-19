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

#include <cstdint>
#include <ranges>
#include <unordered_map>

namespace mctp_socket
{

using FileDesc = int;

/** @class Manager
 *
 *  The Manager class provides API to register MCTP endpoints and the socket to
 *  communicate with the endpoint. The lookup APIs are used when processing MCTP
 *  VDM Rx messages and when sending MCTP VDM Tx messages.
 */
class Manager
{
  public:
    Manager() = default;
    Manager(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    /** @brief Register MCTP endpoint
     *
     *  @param[in] eid - MCTP endpoint ID
     *  @param[in] fd - File descriptor of MCTP socket to do Tx/Rx
     *                  with the MCTP endpoint ID
     */
    void registerEndpoint(uint8_t eid, FileDesc fd)
    {
        eidToFd[eid] = fd;
    }

    /** @brief Get the MCTP socket file descriptor associated with the endpoint
     *
     *  @param[in] eid - MCTP endpoint ID
     *
     *  @return MCTP socket file descriptor, or -1 if EID is not registered
     */
    int getSocket(uint8_t eid) const
    {
        auto it = eidToFd.find(eid);
        if (it == eidToFd.end())
        {
            return -1;
        }
        return it->second;
    }

    /** @brief Clear all the MCTP endpoints
     *
     */
    inline void clearMctpEndpoints()
    {
        eidToFd.clear();
    }

    /** @brief Clear the MCTP endpoint to Fd mapping for a given EID
     * indicating that the socket is no longer being used by the EID
     *
     *  @param[in] eid - MCTP endpoint ID
     *
     */
    inline void clearMctpEndpoint(uint8_t eidToErase)
    {
        eidToFd.erase(eidToErase);
    }

    /** @brief Returns the list of EIDs with unfulfilled requests
     *
     */
    auto getActiveEndpoints()
    {
        return std::views::keys(eidToFd);
    }

  private:
    /** @brief Map of endpoint IDs to socket fd*/
    std::unordered_map<uint8_t, FileDesc> eidToFd;
};

} // namespace mctp_socket