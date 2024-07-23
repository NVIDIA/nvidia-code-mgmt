#pragma once

#include "socket_handler.hpp"
#include "types.hpp"

#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace mctp_vdm
{

using namespace dbus;

/** @class MctpDiscoveryHandlerIntf
 *
 * This abstract class defines the APIs for MctpDiscovery class has common
 * interface to execute function from different manager classes
 */
class MctpDiscoveryHandlerIntf
{
  public:
    virtual void handleMctpEndpoints(const mctp::Infos& mctpInfos) = 0;
    virtual ~MctpDiscoveryHandlerIntf()
    {}
};

class MctpDiscovery
{
  public:
    MctpDiscovery() = delete;
    MctpDiscovery(const MctpDiscovery&) = delete;
    MctpDiscovery(MctpDiscovery&&) = delete;
    MctpDiscovery& operator=(const MctpDiscovery&) = delete;
    MctpDiscovery& operator=(MctpDiscovery&&) = delete;
    ~MctpDiscovery() = default;

    /** @brief Constructs the MCTP Discovery object to handle discovery of
     *         MCTP enabled devices
     *
     *  @param[in] bus - reference to systemd bus
     *  @param[in] list - initializer list to the MctpDiscoveryHandlerIntf
     */
    explicit MctpDiscovery(
        sdbusplus::bus::bus& bus, mctp_socket::Handler& handler,
        std::initializer_list<MctpDiscoveryHandlerIntf*> list);

  private:
    /** @brief reference to the systemd bus */
    sdbusplus::bus::bus& bus;

    /** @brief Used to watch for new MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointAddedSignal;

    mctp_socket::Handler& handler;

    void discoverEndpoints(sdbusplus::message::message& msg);

    /** @brief Process the D-Bus MCTP endpoint info and prepare data to be used
     *         for MCTP VDM discovery.
     *
     *  @param[in] interfaces - MCTP D-Bus information
     *  @param[out] mctpInfos - MCTP info for MCTP VDM discovery
     */
    void populateMctpInfo(const dbus::InterfaceMap& interfaces,
                          mctp::Infos& mctpInfos);

    /** @brief MCTP endpoint interface name */
    const std::string mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};

    /** @brief Unix Socket interface name */
    static constexpr std::string_view unixSocketIntfName{
        "xyz.openbmc_project.Common.UnixSocket"};

    std::vector<MctpDiscoveryHandlerIntf*> handlers;

    /** @brief Helper function to invoke registered handlers
     *
     *  @param[in] mctpInfos - information of discovered MCTP endpoints
     */
    void handleMctpEndpoints(const mctp::Infos& mctpInfos);
};

} // namespace mctp_vdm
