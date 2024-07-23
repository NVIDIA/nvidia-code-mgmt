#include "mctp_discovery_resource.hpp"


std::unordered_set<std::string> MCTPDiscoveryResource::getMctpServices() const noexcept
{
    nvidia::software::updater::GetSubTreeResponse getSubTreeResponse{};
    std::unordered_set<std::string> mctpCtrlServices{};
    const nvidia::software::updater::Interfaces ifaceList{mctpEndpointIntfName};
    try
    {
        auto method = bus.new_method_call(mapperService, mapperPath,
                                          mapperInterface, "GetSubTree");
        method.append("/xyz/openbmc_project/mctp", 0, ifaceList);
        auto reply = bus.call(method);
        reply.read(getSubTreeResponse);
    }
    catch (const std::exception& e)
    {
        lg2::error("D-Bus error calling Subtrees method on ObjectMapper: {ERROR}",
                "ERROR", e.what());
    }

    for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
    {
        for (const auto& [service, interfaces] : mapperServiceMap)
        {
            mctpCtrlServices.insert(service);
        }
    }

    return mctpCtrlServices;
}

std::unordered_map<std::string, std::string> MCTPDiscoveryResource::getMCTPObjects()
{
    std::unordered_map<std::string, std::string> mctpObjects{};
    const auto& mctpCtrlServices = getMctpServices();
    for (const auto& serviceName : mctpCtrlServices)
    {
        auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
        const auto objects = dbusUtil.getManagedObjects(serviceName.c_str(),  "/xyz/openbmc_project/mctp");

        for (const auto& [objectPath, interfaces] : objects)
        {
            if (!interfaces.contains(uuidIntfName))
            {
                continue;
            }
            const auto& mctpUUID = std::get<std::string>(interfaces.at(uuidIntfName).at("UUID"));
            if (mctpUUID.c_str() != uuid)
            {
                continue;
            }
            mctpObjects[serviceName] = objectPath;
            break;
        }
    }
    return mctpObjects;
}

void MCTPDiscoveryResource::startWatchingMCTPObjects()
{
    mctpEidObjects = getMCTPObjects();
    if (mctpEidObjects.empty())
    {
        mctpObjManagerMatch.emplace_back(bus, MatchRules::interfacesAdded("/xyz/openbmc_project/mctp"),
                  [&]([[maybe_unused]] sdbusplus::message::message& msg)
                  {
                      startWatchingMCTPObjects();
                  });
        return;
    }

    mctpObjManagerMatch.clear();

    for (const auto& [service, mctpObject]: mctpEidObjects)
    {
        deviceMatches.emplace_back(bus, MatchRules::propertiesChanged(mctpObject.c_str(),
            mctpEndpointEnableIntfName),
        std::bind(&MCTPDiscoveryResource::onMCTPDiscoveryMsg, this,
                  std::placeholders::_1));
        deviceMatches.emplace_back(bus, MatchRules::interfacesAdded(mctpObject.c_str()),
        std::bind(&MCTPDiscoveryResource::onMCTPDiscoveryMsg, this,
                  std::placeholders::_1));
    }
}

bool MCTPDiscoveryResource::checkForEnabledMCTPEids() const noexcept
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    bool ret = false;
    for (const auto& [service, mctpObject]: mctpEidObjects)
    {
        ret = ret or dbusUtil.getProperty<bool>(service.c_str(), mctpObject.c_str(),
                mctpEndpointEnableIntfName, "Enabled");
    }
    return ret;
}
