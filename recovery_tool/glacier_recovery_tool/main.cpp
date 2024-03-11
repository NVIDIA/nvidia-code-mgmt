#include "config.h"

#include "glacier_recovery_commands.hpp"
#include "message_registry.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>

using json = nlohmann::json;
using namespace phosphor::logging;
using RecoveryResult =
    glacier_recovery_tool::glacier_recovery_commands::RecoveryResult;

static constexpr uint8_t delay1sec = 1;

json loadJSONFile(const std::filesystem::path& path)
{
    std::ifstream ifs(path);

    if (!ifs)
    {
        throw std::runtime_error("Unable to open file PATH=" + path.string());
    }
    try
    {
        return nlohmann::json::parse(ifs);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("Failed to parse JSON PATH=" + path.string() +
                                 ", REASON: " + e.what());
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        lg2::error("Invalid number of arguments");
        return -1;
    }
    json configData = {};
    try
    {
        std::filesystem::path configJsonPath =
            "/usr/share/nvidia-code-mgmt/glacier_recovery_config.json";
        configData = loadJSONFile(configJsonPath);
    }
    catch (const std::exception& e)
    {
        lg2::error(e.what());
        return -1;
    }
    int recoveryTaskState = 0;
    auto bus = sdbusplus::bus::new_default();
    std::unique_ptr<MessageRegistry> messageRegistry =
        std::make_unique<MessageRegistry>(bus);
    for (const auto& [device, i2cAddMap] : configData.items())
    {
        try
        {
            const auto busAdd = i2cAddMap.at("busAddr").get<int>();
            const auto slaveAdd = i2cAddMap.at("slaveAddr").get<int>();

            auto glacierRecoveryObj = std::make_unique<
                glacier_recovery_tool::glacier_recovery_commands::
                    GlacierRecoveryCommands>(busAdd, slaveAdd, false);

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
