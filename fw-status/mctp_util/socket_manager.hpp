#pragma once

#include "types.hpp"

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
     *  @param[in] fd - File descriptor of MCTP demux daemon socket to do Tx/Rx
     *                  with the MCTP endpoint ID
     */
    void registerEndpoint(uint8_t eid, FileDesc fd)
    {
        eidToFd[eid] = fd;
    }

    /** @brief Get the MCTP demux daemon socket file descriptor associated with
     *         the uint8_t
     *
     *  @param[in] eid - MCTP endpoint ID
     *
     *  @return MCTP demux daemon's file descriptor
     */
    int getSocket(uint8_t eid)
    {
        return eidToFd[eid];
    }

    /** @brief Clear all the MCTP endpoints
     *
     */
    void clearMctpEndpoints()
    {
        eidToFd.clear();
    }

  private:
    /** @brief Map of endpoint IDs to socket fd*/
    std::unordered_map<uint8_t, FileDesc> eidToFd;
};

} // namespace mctp_socket