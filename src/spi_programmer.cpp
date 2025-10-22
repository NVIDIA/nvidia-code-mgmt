
#include "config.h"

#include "spi_programmer.hpp"

#include <sdbusplus/asio/connection.hpp>

namespace nvidia
{
namespace software
{
namespace updater
{
std::string SPIProgrammer::getVersion(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    // return the target firmware name
    return inventoryMap.at(inventoryPath).second;
}

std::string SPIProgrammer::getManufacturer(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string SPIProgrammer::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

bool SPIProgrammer::isSpiObjectPresent()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    const auto managedObjects =
        dbusUtil.getManagedObjects(entityManagerService, inventoryRootPath);
    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(spiObjectInterfaces))
        {
            return true;
        }
    }
    return false;
}

void SPIProgrammer::populateSpiSwObjects()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    const auto managedObjects =
        dbusUtil.getManagedObjects(entityManagerService, inventoryRootPath);

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(spiObjectInterfaces))
        {
            const auto targetFirmware =
                getString(interfaces, spiObjectInterfaces, "TargetFirmware");
            if (!targetFirmware.empty())
            {
                const auto chassisName = emObjectPath.parent_path().filename();
                inventoryMap[emObjectPath.str] =
                    std::make_pair(chassisName, targetFirmware);
            }
        }
    }

    // populate the SPI Software objects
    readExistingFirmWare();
}

void SPIProgrammer::tryPopulateSpiSwObjects()
{
    // If no SPI objects are present, add a match rule to populate the objects
    if (!isSpiObjectPresent())
    {
        lg2::info(
            "SPIProgrammer:: No SPI objects present, adding match rule to populate the objects");
        inventoryObjectMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus, MatchRules::interfacesAdded(inventoryRootPath),
            [this]([[maybe_unused]] sdbusplus::message::message& msg) {
                sdbusplus::message::object_path objPath;
                std::map<std::string, std::map<std::string, Value>> interfaces;
                msg.read(objPath, interfaces);

                for (const auto& [interfaceName, properties] : interfaces)
                {
                    if (interfaceName == spiObjectInterfaces)
                    {
                        populateSpiSwObjects();
                        inventoryObjectMatch.reset();
                        break;
                    }
                }
            });
    }
    else
    {
        populateSpiSwObjects();
    }
}

} // namespace updater
} // namespace software
} // namespace nvidia
