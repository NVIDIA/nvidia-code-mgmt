#include "config.h"

#include "glacier_recovery_commands.hpp"
#include "message_registry.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include "dbusutils.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>

using namespace phosphor::logging;
using RecoveryResult =
    glacier_recovery_tool::glacier_recovery_commands::RecoveryResult;
constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto glacierCrisisObjInterface = "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto gpioObjInterface = "xyz.openbmc_project.Configuration.GPIORecovery";

static constexpr uint8_t delay1sec = 1;

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

// Check if it is a Glacier device and assign the interface it uses
static bool isGlacierDevice(nvidia::software::updater::InterfaceMap interfaces,
                        std::string& interface)
{
    if (interfaces.contains(glacierCrisisObjInterface))
    {
        bool isRecoverable{true};
        // Check if device is recoverable (i.e., ERoT)
        if (interfaces.at(glacierCrisisObjInterface).find("isRecoverable") != interfaces.at(glacierCrisisObjInterface).end())
        {
            interface = glacierCrisisObjInterface;
            isRecoverable = std::get<bool>(interfaces.at(glacierCrisisObjInterface).at("isRecoverable"));
            return isRecoverable;
        }
        if (!isRecoverable)
        {
            return false;
        }
    }
    else if (interfaces.contains(gpioObjInterface) &&
                std::get<bool>(interfaces.at(gpioObjInterface).at("IsERoT")))
    {
        interface = gpioObjInterface;
        return true;
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        lg2::error("Invalid number of arguments");
        return -1;
    }
    auto& bus = getBus();
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects = dbusUtil.getManagedObjects(entityManagerService, entityManagerObjManager);
    std::unique_ptr<MessageRegistry> messageRegistry =
        std::make_unique<MessageRegistry>(bus);
    if (managedObjects.empty())
    {
        lg2::error("No Devices found to recover");
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
            static_cast<ErrorCode>(noDevicesFound), "GlacierCrisisRecovery");
        return -1;
    }

    int recoveryTaskState = 0;
    std::string interface{};
    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        interface = "";
        if (!isGlacierDevice(interfaces, interface))
        {
            continue;
        }

        lg2::info("Found Glacier Crisis recovery config Object: {PATH}", "PATH", emObjectPath);
        const auto [busAdd, slaveAdd] = getI2CBusAndAddress(emObjectPath, interface);
        const auto& device = emObjectPath.filename();
        try
        {
            auto glacierRecoveryObj = std::make_unique<
                glacier_recovery_tool::glacier_recovery_commands::
                    GlacierRecoveryCommands>(busAdd, slaveAdd, false);

            auto isHiddenByFPGA = std::get<bool>(interfaces.at(interface).at("HiddenByFPGA"));
            if (isHiddenByFPGA)
            {
                if (!glacierRecoveryObj->unlockI2CDevice())
                {
                    lg2::info(
                        "Failed to unlock addresses for I2C device. Device: {DEVICE}",
                        "DEVICE", device);
                    messageRegistry->createMessageRegistryResourceErrors(
                        resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
                        static_cast<ErrorCode>(deviceNotResponding), device);
                    continue;
                }
            }

            auto initRes = glacierRecoveryObj->performInitialization();
            if (initRes != RecoveryResult::Ok)
            {
                if (initRes == RecoveryResult::FirmwareNotInRecovery)
                {
                    lg2::info("Device {DEVICE} is not in recovery state",
                              "DEVICE", device);
                    messageRegistry->createMessageRegistry(firmwareNotInRecovery,
                                                           device);
                }
                else
                {
                    lg2::error(
                        "Firmware Recovery Initialization failed for Device: {DEVICE}, Error: {ERROR}",
                        "DEVICE", device, "ERROR",
                        glacierRecoveryObj->recoveryResultToStr(initRes));
                    messageRegistry->createMessageRegistryResourceErrors(
                        resourceErrorsDetected,
                        RecoveryProtocol::GlacierRecovery,
                        static_cast<ErrorCode>(initRes), device);
                    recoveryTaskState = -1;
                }
                continue;
            }
            lg2::info(
                "Device {DEVICE} is in recovery state, Firmware recovery is started.",
                "DEVICE", device);
            messageRegistry->createMessageRegistry(recoveryStarted, device);
            auto imgPath = argv[1];

            /*
             * There is a timing issue happens if we perform the recovery on ERoTs on the same bus
             * consecutively. Add 1 sec sleep here as a workaround
             */
            sleep(1);
            auto recResult =
                glacierRecoveryObj->performGlacierRecovery(imgPath);
            if (recResult != RecoveryResult::Ok)
            {
                lg2::error(
                    "Firmware Recovery failed for Device: {DEVICE}, Error: {ERROR}",
                    "DEVICE", device, "ERROR",
                    glacierRecoveryObj->recoveryResultToStr(recResult));
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
                    static_cast<ErrorCode>(recResult), device);
                recoveryTaskState = -1;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
            auto postRecInitRes = glacierRecoveryObj->performInitialization();
            if (postRecInitRes != RecoveryResult::Ok)
            {
                if (postRecInitRes == RecoveryResult::FirmwareNotInRecovery)
                {
                    lg2::info("Device {DEVICE} is successfully recovered",
                              "DEVICE", device);
                    messageRegistry->createMessageRegistry(recoverySuccessful,
                                                           device);
                }
                else
                {
                    lg2::error(
                        "Firmware recovery is performed on device: {DEVICE}, Error while getting status, Error: {ERROR}",
                        "DEVICE", device, "ERROR",
                        glacierRecoveryObj->recoveryResultToStr(
                            postRecInitRes));
                    messageRegistry->createMessageRegistryResourceErrors(
                        resourceErrorsDetected,
                        RecoveryProtocol::GlacierRecovery,
                        static_cast<ErrorCode>(postRecInitRes), device);
                    recoveryTaskState = -1;
                }
            }
            else
            {
                lg2::error(
                    "Firmware recovery failed, Device {DEVICE} is still in recovery state.",
                    "DEVICE", device);
                messageRegistry->createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
                    deviceRecoveryFailed, device);
                recoveryTaskState = -1;
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Firmware Recovery failed for Device: {DEVICE}, Error: {ERROR}",
                "DEVICE", device, "ERROR", e.what());
            messageRegistry->createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
                deviceRecoveryFailed, device);
            recoveryTaskState = -1;
        }
    }
    return recoveryTaskState;
}
