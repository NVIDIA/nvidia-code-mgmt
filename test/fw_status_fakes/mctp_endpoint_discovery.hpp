#pragma once

#include "../../fw-status/mctp_util/types.hpp"

namespace mctp_vdm
{

class InstanceIdMgr
{
  public:
    InstanceIdMgr() = default;
};

namespace requester
{

class Handler
{
  public:
    template <typename... Args>
    explicit Handler(Args&&...)
    {}
};

} // namespace requester

class MctpDiscoveryHandlerIntf
{
  public:
    virtual ~MctpDiscoveryHandlerIntf() = default;
    virtual void handleMctpEndpoints([[maybe_unused]] const mctp::Infos&)
    {}
};

class MctpDiscovery
{
  public:
    template <typename... Args>
    explicit MctpDiscovery(Args&&...)
    {}
};

} // namespace mctp_vdm

namespace mctp_socket
{

class Manager
{
  public:
    Manager() = default;
};

class Handler
{
  public:
    template <typename... Args>
    explicit Handler(Args&&...)
    {}
};

} // namespace mctp_socket
