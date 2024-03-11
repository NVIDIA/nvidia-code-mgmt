#include "recovery_commands.hpp"
#include <phosphor-logging/lg2.hpp>
#include "recovery_commandline.hpp"

namespace ocp_recovery_commandline
{

OCPRecoveryCommandLine::OCPRecoveryCommandLine(const std::string_view device,
        int busAddr, int slaveAddr, bool verb, bool emul) :
    verbose(verb),
    device(device),
    recoveryCommands(std::make_unique<
            recovery_tool::recovery_commands::OCPRecoveryCommands>(busAddr, slaveAddr, verb, emul)),
    registry(bus)
{
}

DeviceStatus OCPRecoveryCommandLine::getDeviceStatus() const noexcept
{
    lg2::info("Getting Device Status");
    const auto& [success, hexData, errorMsg] =
        recoveryCommands->getDeviceStatusCommand();
    if (!success)
    {
        lg2::error(std::string("Error while getting Device Status: " + errorMsg).c_str());
        return {DeviceStatusCode::CommandFailure, RecoveryReasonCode::BFNF, ProtocolError::GeneralProtocolError};
    }
    return {static_cast<DeviceStatusCode>(hexData[1]), static_cast<RecoveryReasonCode>(hexData[4] << 8 | hexData[3]),
        static_cast<ProtocolError>(hexData[2])};
}

RecoveryStatus OCPRecoveryCommandLine::getRecoveryStatus() const noexcept
{
    auto [success, hexData, errMsg] =
        recoveryCommands->getRecoveryStatusCommand();

    if (!success)
    {
        lg2::error(std::string("Error while getting Recovery Status: " + errMsg).c_str());
        return RecoveryStatus::CommandFailure;
    }
    return static_cast<RecoveryStatus>(hexData[1]);
}

RecoveryReturnCode OCPRecoveryCommandLine::performRecovery(
        const std::vector<std::string>& imagePaths) const noexcept
{
    DeviceStatus response = getDeviceStatus();
    if (response.statusCode == DeviceStatusCode::CommandFailure)
    {
        lg2::error(
            "Error in getting device status, Recovery can't proceed.");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryProtocolError,
           static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
           device);
        return RecoveryReturnCode::SKIPPED;
    }

    if (response.statusCode != DeviceStatusCode::RecoveryMode)
    {
        lg2::error("Device is not in recovery mode.");
        if (response.protocolError != ProtocolError::NoProtocolError)
        {
             registry.createMessageRegistryResourceErrors(
                resourceErrorsDetected,
                RecoveryProtocol::OCPRecoveryProtocolError,
                static_cast<ErrorCode>(response.protocolError),
                device);
        }
        else
        {
             registry.createMessageRegistryResourceErrors(
                firmwareNotInRecovery,
                RecoveryProtocol::OCPRecovery,
                static_cast<ErrorCode>(response.recoveryReason),
                device);
        }
        return RecoveryReturnCode::SKIPPED;
    }

    RecoveryStatus recoveryStatus = getRecoveryStatus();

    if (recoveryStatus == RecoveryStatus::CommandFailure)
    {
        lg2::error(
            "Error in getting device status, Recovery can't proceed");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryProtocolError,
           static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    if (recoveryStatus != RecoveryStatus::AwaitingRecoveryImg)
    {
        lg2::error("Device is not ready to receive recovery images");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryStatusError,
           static_cast<ErrorCode>(recoveryStatus),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    lg2::info("Perform OCP Recovery Task Started.");
    registry.createMessageRegistry(
            recoveryStarted,
            device);

    auto [success, errorMsg] =
        recoveryCommands->performRecoveryCommand(imagePaths);
    if (!success)
    {
        lg2::error(errorMsg.c_str());
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryProtocolError,
           static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    // Adding 1sec delay since probing the status right after
    // recovery gives incosistent results
    std::this_thread::sleep_for(std::chrono::seconds(delay1sec));

    DeviceStatus postRecoveryResponse = getDeviceStatus();
    if (postRecoveryResponse.statusCode == DeviceStatusCode::CommandFailure)
    {
        lg2::error("Error in getting device status");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryProtocolError,
           static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    if (postRecoveryResponse.statusCode != DeviceStatusCode::DeviceHealthy and
            postRecoveryResponse.statusCode != DeviceStatusCode::RecoveryImgRunning)
    {
        lg2::error("Device status is not Healthy.");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecovery,
           static_cast<ErrorCode>(postRecoveryResponse.recoveryReason),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    RecoveryStatus postRecoveryStatusResponse = getRecoveryStatus();
    if (postRecoveryStatusResponse != RecoveryStatus::RecoverySuccess and
           postRecoveryStatusResponse != RecoveryStatus::BootingRecoveryImg)
    {
        lg2::error("Device Recovery not succesful");
        registry.createMessageRegistryResourceErrors(
           resourceErrorsDetected,
           RecoveryProtocol::OCPRecoveryStatusError,
           static_cast<ErrorCode>(postRecoveryStatusResponse),
           device);
        return RecoveryReturnCode::FAILURE;
    }

    lg2::info("Recovery Image Activated.\nPerform Recovery Task Successful.");
    registry.createMessageRegistry(
            recoverySuccessful,
            device);
    return RecoveryReturnCode::SUCCESS;
}

} // namespace recovery_tool

