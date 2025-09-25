
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
    lg2::info("Getting version for {INVENTORY_PATH}", "INVENTORY_PATH",
              inventoryPath);
    return "";
}

std::string SPIProgrammer::getModel(
    [[maybe_unused]] const std::string& inventoryPath) const
{
    lg2::info("Getting model for {INVENTORY_PATH}", "INVENTORY_PATH",
              inventoryPath);
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
            const auto name =
                getString(interfaces, spiObjectInterfaces, "Name");

            lg2::info("SPI Programmer: {NAME}", "NAME", name);

            // check if name contains "BootSPI" as it is the only SPI device
            // that stores the firmware
            if (name.find("BootSPI") != std::string::npos)
            {
                const auto targetFirmware = getString(
                    interfaces, spiObjectInterfaces, "TargetFirmware");
                lg2::info("Target Firmware: {TARGET_FIRMWARE}",
                          "TARGET_FIRMWARE", targetFirmware);
                inventoryMap[emObjectPath.str] =
                    std::make_pair(name, targetFirmware);
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
