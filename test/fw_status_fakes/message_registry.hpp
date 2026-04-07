#pragma once

#include <sdbusplus/bus.hpp>

class MessageRegistry
{
  public:
    explicit MessageRegistry(sdbusplus::bus::bus&)
    {}
};
