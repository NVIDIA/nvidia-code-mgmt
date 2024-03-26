#pragma once

#include "config.h"
#include "orin.hpp"

#include <sdbusplus/bus/match.hpp>
#include <sdeventplus/event.hpp>

using namespace nvidia::orin::device;

namespace nvidia::orin::manager
{

class ORINManager
{
  public:
    ORINManager() = delete;
    ~ORINManager() = default;
    ORINManager(const ORINManager&) = delete;
    ORINManager& operator=(const ORINManager&) = delete;
    ORINManager(ORINManager&&) = delete;
    ORINManager& operator=(ORINManager&&) = delete;

    ORINManager(sdbusplus::bus::bus& bus, std::string basePath);

  private:
    sdbusplus::bus::bus& bus;

    std::unique_ptr<Orin> orinInvs;
};

} // namespace nvidia::orin::manager

