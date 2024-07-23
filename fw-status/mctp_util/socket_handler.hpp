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
 *  The Handler class abstracts the communication with multiple MCTP Tx/Rx
 *  daemons which supports different transport mechanisms. The initialisation of
 *  this class is driven by the discovery of MCTP.Endpoint interface which
 *  exposes the socket information to communicate with the endpoints.  This
 *  manager class handles the data to be read on the communication sockets by
 *  registering callbacks for EPOLLIN.
 */
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
    explicit Handler(
        sdeventplus::Event& event,
        mctp_vdm::requester::Handler<mctp_vdm::requester::Request>& handler,
        mctp_socket::Manager& manager) :
        event(event),
        handler(handler), manager(manager)
    {}

    void registerMctpEndpoint(uint8_t eid, int type, int protocol,
                              const std::vector<uint8_t>& pathName);
    int activateSockets(const std::vector<uint8_t>& eids);
    void deactivateSockets();

  private:
    sdeventplus::Event& event;
    mctp_vdm::requester::Handler<mctp_vdm::requester::Request>& handler;
    mctp_socket::Manager& manager;
    int initSocket(int type, int protocol,
                   const std::vector<uint8_t>& pathName);

    void processRxMsg(const std::vector<uint8_t>& requestMsg);

    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<std::vector<uint8_t>,
             std::tuple<std::unique_ptr<utils::CustomFD>, std::unique_ptr<IO>>>
        socketInfoMap;

    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<uint8_t, std::tuple<int, int, std::vector<uint8_t>>> eidToSockMap;
};

} // namespace mctp_socket