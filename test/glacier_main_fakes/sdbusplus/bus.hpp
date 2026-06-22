#pragma once

namespace sdbusplus::bus
{

class bus
{};

inline bus new_default()
{
    return {};
}

} // namespace sdbusplus::bus

namespace sdbusplus
{
using bus_t = bus::bus;
}
