#include "recovery_commandline.hpp"

#include "recovery_commands.hpp"

#include <phosphor-logging/lg2.hpp>

namespace ocp_recovery_commandline
{

OCPRecoveryCommandLine::OCPRecoveryCommandLine(const std::string_view device,
                                               int busAddr, int slaveAddr,
                                               bool verb, bool emul) :
    verbose(verb), device(device),
    recoveryCommands(
        std::make_unique<recovery_tool::recovery_commands::OCPRecoveryCommands>(
            busAddr, slaveAddr, verb, emul)),
    registry(bus)
{}

DeviceStatus OCPRecoveryCommandLine::getDeviceStatus() const noexcept
{
    lg2::info("Getting Device Status");

    try
    {
        const auto& [success, hexData, errorMsg] =
            recoveryCommands->getDeviceStatusCommand();
        if (success)
        {
            return {
                static_cast<DeviceStatusCode>(hexData[1]),
                static_cast<RecoveryReasonCode>(hexData[4] << 8 | hexData[3]),
                static_cast<ProtocolError>(hexData[2])};
        }
        lg2::error("Error while getting Device Status: {ERROR}", "ERROR",
                   errorMsg);
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception while getting Device Status: {ERROR}", "ERROR",
                   e.what());
    }

    return {DeviceStatusCode::CommandFailure, RecoveryReasonCode::BFNF,
            ProtocolError::DeviceNotResponding};
}

std::pair<RecoveryStatus, uint8_t>
    OCPRecoveryCommandLine::getRecoveryStatus() const noexcept
{
    auto [success, hexData, errMsg] =
        recoveryCommands->getRecoveryStatusCommand();

    try
    {
        if (success)
        {
            // Extract recovery status from bits [3:0] of hexData[1]
            constexpr uint8_t recoveryStatusMask = 0x0F;
            uint8_t recoveryStatus = hexData[1] & recoveryStatusMask;

            // Extract recovery image index from bits [7:4] of hexData[1]
            constexpr uint8_t recoveryImageIndexMask = 0xF0;
            constexpr uint8_t recoveryImageIndexShift = 4;
            uint8_t recoveryImageIndex =
                (hexData[1] & recoveryImageIndexMask) >>
                recoveryImageIndexShift;

            return {static_cast<RecoveryStatus>(recoveryStatus),
                    recoveryImageIndex};
        }
        lg2::error("Error while getting Recovery Status: {ERROR}", "ERROR",
                   errMsg);
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception while getting Recovery Status: {ERROR}", "ERROR",
                   e.what());
    }
    return {RecoveryStatus::CommandFailure, 0};
}

std::string OCPRecoveryCommandLine::deviceStatusToStr(
    DeviceStatusCode status) const noexcept
{
    switch (status)
    {
        case DeviceStatusCode::StatusPending:
            return "Status Pending";
        case DeviceStatusCode::DeviceHealthy:
            return "Device healthy";
        case DeviceStatusCode::DeviceError:
            return "Device Error";
        case DeviceStatusCode::RecoveryMode:
            return "Recovery mode";
        case DeviceStatusCode::RecoveryPending:
            return "Recovery Pending";
        case DeviceStatusCode::RecoveryImgRunning:
            return "Running Recovery Image";
        case DeviceStatusCode::BootFailure:
            return "Boot Failure";
        case DeviceStatusCode::FatalError:
            return "Fatal Error";
        default:
            return "Reserved/Unknown";
    }
}

std::string OCPRecoveryCommandLine::protocolErrorToStr(
    ProtocolError error) const noexcept
{
    switch (error)
    {
        case ProtocolError::NoProtocolError:
            return "No Protocol Error";
        case ProtocolError::UnsupportedWriteCommand:
            return "Unsupported/Write Command";
        case ProtocolError::UnsupportedParameter:
            return "Unsupported Parameter";
        case ProtocolError::LengthWriteError:
            return "Length write error";
        case ProtocolError::CrcError:
            return "CRC Error";
        case ProtocolError::GeneralProtocolError:
            return "General Protocol Error";
        case ProtocolError::DeviceNotResponding:
            return "Device Not Responding";
        default:
            return "Reserved/Unknown";
    }
}

std::string OCPRecoveryCommandLine::recoveryReasonCodeToStr(
    RecoveryReasonCode code) const noexcept
{
    switch (code)
    {
        case RecoveryReasonCode::BFNF:
            return "No Boot Failure detected";
        case RecoveryReasonCode::BFGHWE:
            return "Generic hardware error";
        case RecoveryReasonCode::BFGSE:
            return "Generic hardware soft error - soft error may be recoverable";
        case RecoveryReasonCode::BFSTF:
            return "Self-test failure (e.g., RSA self test failure, FIPs self test failure,, etc.)";
        case RecoveryReasonCode::BFCD:
            return "Corrupted/missing critical data";
        case RecoveryReasonCode::BFKMMC:
            return "Missing/corrupt key manifest";
        case RecoveryReasonCode::BFKMAF:
            return "Authentication Failure on key manifest";
        case RecoveryReasonCode::BFKIAR:
            return "Anti-rollback failure on key manifest";
        case RecoveryReasonCode::BFFIMC:
            return "Missing/corrupt boot loader (first mutable code) firmware image";
        case RecoveryReasonCode::BFFIAF:
            return "Authentication failure on boot loader (1st mutable code) firmware image";
        case RecoveryReasonCode::BFFIAR:
            return "Anti-rollback failure boot loader (1st mutable code) firmware image";
        case RecoveryReasonCode::BFMFMC:
            return "Missing/corrupt main/management firmware image";
        case RecoveryReasonCode::BFMFAF:
            return "Authentication Failure main/management firmware image";
        case RecoveryReasonCode::BFMFAR:
            return "Anti-rollback Failure main/management firmware image";
        case RecoveryReasonCode::BFRFMC:
            return "Missing/corrupt recovery firmware";
        case RecoveryReasonCode::BFRFAF:
            return "Authentication Failure recovery firmware";
        case RecoveryReasonCode::BFRFAR:
            return "Anti-rollback Failure on recovery firmware";
        case RecoveryReasonCode::FR:
            return "Forced Recovery";
        default:
            if (code >= RecoveryReasonCode::ReservedStart &&
                code <= RecoveryReasonCode::ReservedEnd)
            {
                return "Reserved";
            }
            else if (code >= RecoveryReasonCode::VendorUniqueStart &&
                     code <= RecoveryReasonCode::VendorUniqueEnd)
            {
                return "Vendor Unique Boot Failure Code";
            }
            else
            {
                return "Unknown";
            }
    }
}

std::tuple<OperationalStatus, DeviceStatusCode, ProtocolError, RecoveryStatus>
    OCPRecoveryCommandLine::getOperationalStatus() const noexcept
{
    auto deviceStatus = getDeviceStatus();
    if (deviceStatus.statusCode == DeviceStatusCode::CommandFailure)
    {
        return {OperationalStatus::Unreachable,
                DeviceStatusCode::CommandFailure, deviceStatus.protocolError,
                RecoveryStatus::NotInRecoveryMode};
    }

    auto [recoveryStatus, recoveryImageIndex] = getRecoveryStatus();
    if (recoveryStatus == RecoveryStatus::CommandFailure)
    {
        return {OperationalStatus::Unreachable, deviceStatus.statusCode,
                ProtocolError::DeviceNotResponding,
                RecoveryStatus::NotInRecoveryMode};
    }

    lg2::info("Recovery Status for {DEVICE}: status={STATUS}, imageIndex={IDX}",
              "DEVICE", device, "STATUS", static_cast<int>(recoveryStatus),
              "IDX", static_cast<int>(recoveryImageIndex));

    if (deviceStatus.statusCode == DeviceStatusCode::DeviceHealthy or
        (deviceStatus.statusCode == DeviceStatusCode::RecoveryImgRunning and
         (recoveryStatus == RecoveryStatus::BootingRecoveryImg or
          recoveryStatus == RecoveryStatus::RecoverySuccess)))
    {
        return {OperationalStatus::Operational, deviceStatus.statusCode,
                ProtocolError::NoProtocolError,
                RecoveryStatus::NotInRecoveryMode};
    }

    if (deviceStatus.statusCode == DeviceStatusCode::RecoveryMode and
        recoveryStatus == RecoveryStatus::AwaitingRecoveryImg)
    {
        return {OperationalStatus::RecoveryMode, deviceStatus.statusCode,
                ProtocolError::NoProtocolError,
                RecoveryStatus::NotInRecoveryMode};
    }

    lg2::info("Device Status for {DEVICE} is {STATUS}", "DEVICE", device,
              "STATUS", deviceStatusToStr(deviceStatus.statusCode));
    lg2::info("Recovery reason for {DEVICE} is {STATUS}", "DEVICE", device,
              "STATUS", recoveryReasonCodeToStr(deviceStatus.recoveryReason));
    return {OperationalStatus::UnknownState, deviceStatus.statusCode,
            ProtocolError::NoProtocolError, recoveryStatus};
}

RecoveryReturnCode OCPRecoveryCommandLine::performRecovery(
    const std::vector<std::string>& imagePaths) const noexcept
{
    try
    {
        const auto [operationalStatus, deviceStatus, protocolError,
                    recoveryStatus] = getOperationalStatus();

        if (operationalStatus == OperationalStatus::Operational)
        {
            lg2::info("Device {DEVICE} is operational, skipping", "DEVICE",
                      device);
            registry.createMessageRegistryResourceErrors(
                firmwareNotInRecovery, RecoveryProtocol::OCPRecovery,
                static_cast<ErrorCode>(recoveryStatus), device);
            return RecoveryReturnCode::SKIPPED;
        }

        if (operationalStatus == OperationalStatus::Unreachable)
        {
            lg2::info("Device {DEVICE} is unreachable, skipping", "DEVICE",
                      device);
            registry.createMessageRegistryResourceErrors(
                resourceErrorsDetected,
                RecoveryProtocol::OCPRecoveryProtocolError,
                static_cast<ErrorCode>(protocolError), device);
            return RecoveryReturnCode::FAILURE;
        }

        if (operationalStatus == OperationalStatus::UnknownState)
        {
            lg2::error("Device {DEVICE} is in an unknown state. "
                       "Activating force recovery mode",
                       "DEVICE", device);
            auto [ret, _] = recoveryCommands->setForceRecoveryMode();
            if (!ret)
            {
                lg2::error("Unable to activate force recovery mode");
                registry.createMessageRegistryResourceErrors(
                    resourceErrorsDetected,
                    RecoveryProtocol::OCPRecoveryProtocolError,
                    static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
                    device);
                return RecoveryReturnCode::FAILURE;
            }

            std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
            OperationalStatus postForceRecoveryOperationalStatus;
            std::tie(postForceRecoveryOperationalStatus, std::ignore,
                     std::ignore, std::ignore) = getOperationalStatus();
            if (postForceRecoveryOperationalStatus !=
                OperationalStatus::RecoveryMode)
            {
                lg2::error("Device {DEVICE} is not in recovery mode", "DEVICE",
                           device);
                registry.createMessageRegistryResourceErrors(
                    resourceErrorsDetected,
                    RecoveryProtocol::OCPRecoveryProtocolError,
                    static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
                    device);
                return RecoveryReturnCode::FAILURE;
            }
        }

        lg2::info("Perform OCP Recovery Task Started on {DEVICE}.", "DEVICE",
                  device);
        registry.createMessageRegistry(recoveryStarted, device);

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

        const auto [postRecOperationalStatus, postRecDeviceStatus,
                    postRecProtocolError, postRecRecoveryStatus] =
            getOperationalStatus();

        if (postRecOperationalStatus == OperationalStatus::Operational)
        {
            lg2::info(
                "Recovery Image Activated on {DEVICE}.\nPerform Recovery Task Successful.",
                "DEVICE", device);
            registry.createMessageRegistry(recoverySuccessful, device);
            return RecoveryReturnCode::SUCCESS;
        }

        lg2::error("Recovery of {DEVICE} is not successful", "DEVICE", device);
        if (postRecOperationalStatus == OperationalStatus::RecoveryMode or
            postRecDeviceStatus == DeviceStatusCode::BootFailure)
        {
            registry.createMessageRegistryResourceErrors(
                resourceErrorsDetected,
                RecoveryProtocol::OCPRecoveryProtocolError,
                static_cast<ErrorCode>(postRecRecoveryStatus), device);
        }
        else
        {
            registry.createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::OCPDeviceStatusCode,
                static_cast<ErrorCode>(postRecDeviceStatus), device);
        }
        return RecoveryReturnCode::FAILURE;
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception during recovery operation: {ERROR}", "ERROR",
                   e.what());
        registry.createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::OCPRecoveryProtocolError,
            static_cast<ErrorCode>(ProtocolError::GeneralProtocolError),
            device);
        return RecoveryReturnCode::FAILURE;
    }
}

} // namespace ocp_recovery_commandline
