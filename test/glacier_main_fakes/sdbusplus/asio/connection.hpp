#pragma once

#include <boost/asio/io_context.hpp>

namespace sdbusplus::asio
{

class connection
{
  public:
    explicit connection(boost::asio::io_context&)
    {}
};

} // namespace sdbusplus::asio
