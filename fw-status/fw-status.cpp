#include "gpu_resource.hpp"
#include "erot_resource.hpp"

#include "../src/dbusutils.hpp"

#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/event.hpp>

#include <sdbusplus/bus.hpp>
#include <phosphor-logging/lg2.hpp>

#include <iostream>
#include <string_view>
#include <filesystem>

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface = "xyz.openbmc_project.Configuration.OCPRecovery";
constexpr auto glacierCrisisObjInterface = "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto fwStatusService = "com.Nvidia.FWStatus";
constexpr auto fwStatusObjManager = "/xyz/openbmc_project/inventory/system/";
constexpr auto i2cInterface = "xyz.openbmc_project.Inventory.Decorator.I2CDevice";
constexpr auto uuidInterface = "xyz.openbmc_project.Common.UUID";

using namespace phosphor::logging;

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

std::pair<uint32_t, uint32_t> getI2CBusAndAddress(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto i2cBus = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CBus");
    auto i2cAddress = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CAddress");

    return {i2cBus, i2cAddress};
}

std::string getUUID(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto uuid = dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
           interface.c_str(), "UUID");
    return uuid;
}

std::string getSoftwareDBusObjectPath(const auto& path)
{
    std::string name = path.filename();
    return "/xyz/openbmc_project/software/" + name;
}

int main()
{
    auto& bus = getBus();
    sdbusplus::server::manager_t mgr{bus, fwStatusObjManager};

    bus.request_name(fwStatusService);

    nvidia::software::updater::ObjectValueTree managedObjects{};
    try
    {
        nvidia::software::updater::ObjectValueTree tmpObjects{};
        auto method = bus.new_method_call(entityManagerService, entityManagerObjManager,
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        auto reply = bus.call(method);
        reply.read(tmpObjects);
        managedObjects.insert(tmpObjects.begin(), tmpObjects.end());
    }
    catch (const std::exception& e)
    {
        lg2::error("D-Bus error while fetching managed objects for {SERVICE}: {ERROR} ",
                "SERVICE", entityManagerService, "ERROR", e.what());
    }

    std::vector<std::unique_ptr<BaseResource>> resources;

    for (const auto& [emmObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(ocpObjInterface))
        {
            lg2::info("Found OCP recovery config Object: {PATH}", "PATH", emmObjectPath);
            const auto [i2cBus, i2cAddress] = getI2CBusAndAddress(emmObjectPath, ocpObjInterface);
            const auto uuid = getUUID(emmObjectPath, ocpObjInterface);
            const auto objPath = getSoftwareDBusObjectPath(emmObjectPath);
            resources.push_back(std::make_unique<GpuResource>(bus, objPath, i2cBus, i2cAddress, uuid));
        }
        else if (interfaces.contains(glacierCrisisObjInterface))
        {
            lg2::info("Found Glacier Crisis recovery config Object: {PATH}", "PATH", emmObjectPath);
            const auto [i2cBus, i2cAddress] = getI2CBusAndAddress(emmObjectPath, glacierCrisisObjInterface);
            const auto uuid = getUUID(emmObjectPath, glacierCrisisObjInterface);
            const auto objPath = getSoftwareDBusObjectPath(emmObjectPath);
            resources.push_back(std::make_unique<ERoTResource>(bus, objPath, i2cBus, i2cAddress, uuid));
        }
    }

    // Handle dbus processing forever.
    bus.process_loop();
}

