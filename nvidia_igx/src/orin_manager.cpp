#include "orin_manager.hpp"

using namespace nvidia::orin::common;

namespace nvidia::orin::manager
{

ORINManager::ORINManager(sdbusplus::bus::bus& bus, std::string basePath) :
    bus(bus)
{
    try
    {
        const auto baseinvInvPath = basePath + "/" + "ORIN";

        orinInvs = std::make_unique<Orin>(bus, baseinvInvPath); 
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
}

} // namespace nvidia::orin::manager

