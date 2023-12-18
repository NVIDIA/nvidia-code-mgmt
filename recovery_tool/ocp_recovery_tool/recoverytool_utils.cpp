#include "recoverytool_utils.hpp"

namespace recovery_tool
{

OCPRecoveryTool::OCPRecoveryTool(int busAddr, int slaveAddr, bool verb,
                                 bool emul) :
    verbose(verb),
    recoveryCommands(busAddr, slaveAddr, verb, emul)
{}

void OCPRecoveryTool::logVerbose(const std::string& message) const
{
    if (verbose)
    {
        std::cout << message << "\n";
    }
}

std::string OCPRecoveryTool::deviceStatusToStr(DeviceStatus status)
{
    switch (status)
    {
        case DeviceStatus::StatusPending:
            return "Status Pending";
        case DeviceStatus::DeviceHealthy:
            return "Device healthy";
        case DeviceStatus::DeviceError:
            return "Device Error";
        case DeviceStatus::RecoveryMode:
            return "Recovery mode";
        case DeviceStatus::RecoveryPending:
            return "Recovery Pending";
        case DeviceStatus::RecoveryImgRunning:
            return "Running Recovery Image";
        case DeviceStatus::BootFailure:
            return "Boot Failure";
        case DeviceStatus::FatalError:
            return "Fatal Error";
        default:
            return "Reserved/Unknown";
    }
}

std::string OCPRecoveryTool::protocolErrorToStr(ProtocolError error)
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
        default:
            return "Reserved/Unknown";
    }
}

std::string OCPRecoveryTool::recoveryReasonCodeToStr(RecoveryReasonCode code)
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

std::string OCPRecoveryTool::recoveryStatusToStr(RecoveryStatus status)
{
    switch (status)
    {
        case RecoveryStatus::NotInRecoveryMode:
            return "Not in recovery mode";
        case RecoveryStatus::AwaitingRecoveryImg:
            return "Awaiting recovery image";
        case RecoveryStatus::BootingRecoveryImg:
            return "Booting recovery image";
        case RecoveryStatus::RecoverySuccess:
            return "Recovery successful";
        case RecoveryStatus::RecoveryFailed:
            return "Recovery failed";
        case RecoveryStatus::RecoveryImgAuthFailed:
            return "Recovery image authentication error";
        case RecoveryStatus::ErrorEnteringRecoveryMode:
            return "Error entering Recovery mode (might be administratively disabled)";
        case RecoveryStatus::InvalidCms:
            return "Invalid component address space";
        default:
            return "Reserved";
    }
}

nlohmann::json
    OCPRecoveryTool::assignPerformRecoveryError(const std::string& errorMsg)
{
    nlohmann::json response;
    response["Error"] = errorMsg;
    response["Status"] = "Failed";
    return response;
}

nlohmann::json OCPRecoveryTool::getDeviceStatusJson()
{
    nlohmann::json jsonResponse;
    try
    {
        logVerbose("Getting Device Status");
        auto [success, hexData, errorMsg] =
            recoveryCommands.getDeviceStatusCommand();
        if (success)
        {
            jsonResponse["Device Status"] =
                deviceStatusToStr(static_cast<DeviceStatus>(hexData[1]));
            jsonResponse["Protocol Error"] =
                protocolErrorToStr(static_cast<ProtocolError>(hexData[2]));

            // little-endian format
            jsonResponse["Recovery Reason Codes"] = recoveryReasonCodeToStr(
                static_cast<RecoveryReasonCode>(hexData[4] << 8 | hexData[3]));
            jsonResponse["Heartbeat"] = hexData[6] << 8 | hexData[5];

            int vendorStatusLength = hexData[7];
            jsonResponse["Vendor Status Length"] = vendorStatusLength;
            if (vendorStatusLength > 0)
            {
                std::string vendorStatus;
                size_t maxIndex =
                    std::min(static_cast<size_t>(vendorStatusLength),
                             hexData.size() - 8);
                for (size_t i = 0; i < maxIndex; ++i)
                {
                    vendorStatus += std::to_string(hexData[8 + i]) + " ";
                }

                jsonResponse["Vendor Status"] = vendorStatus;
            }
        }
        else
        {
            logVerbose("Error while getting Device Status: " + errorMsg);
            jsonResponse["Error"] = errorMsg;
        }
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        logVerbose("Error in GetDeviceStatus: " + std::string(e.what()));
        jsonResponse["Error"] = e.what();
        return jsonResponse;
    }
}

nlohmann::json OCPRecoveryTool::getRecoveryStatusJson()
{
    nlohmann::json jsonResponse;
    try
    {
        auto [success, hexData, errMsg] =
            recoveryCommands.getRecoveryStatusCommand();

        if (success)
        {
            jsonResponse["Device Recovery Status"] =
                recoveryStatusToStr(static_cast<RecoveryStatus>(hexData[1]));
            jsonResponse["Vendor Specific Status"] = std::to_string(hexData[2]);
        }
        else
        {
            logVerbose("Error while getting Recovery Status: " + errMsg);
            jsonResponse["Error"] = errMsg;
        }
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        logVerbose("Error in GetRecoveryStatus: " + std::string(e.what()));
        jsonResponse["Error"] = e.what();
        return jsonResponse;
    }
}

nlohmann::json
    OCPRecoveryTool::performRecovery(const std::vector<std::string>& imagePaths)
{
    nlohmann::json jsonResponse;
    try
    {
        logVerbose("Perform OCP Recovery Task Started.");

        if (imagePaths.empty())
        {
            logVerbose("Image paths are empty");
            return assignPerformRecoveryError("Image paths are empty");
        }
        auto response = getDeviceStatusJson();
        if (!response.contains("Device Status"))
        {
            logVerbose(
                "Error in getting device status, Recovery can't proceed.");
            return assignPerformRecoveryError("Getting Device Status Failed");
        }

        if (response["Device Status"] !=
            deviceStatusToStr(DeviceStatus::RecoveryMode))
        {
            logVerbose("Device is not in recovery mode.");
            return assignPerformRecoveryError("Device is not in recovery mode");
        }

        response = getRecoveryStatusJson();

        if (!response.contains("Device Recovery Status"))
        {
            logVerbose(
                "Error in getting device status, Recovery can't proceed");
            return assignPerformRecoveryError("Getting Recovery Status Failed");
        }

        if (response["Device Recovery Status"] !=
            recoveryStatusToStr(RecoveryStatus::AwaitingRecoveryImg))
        {
            logVerbose("Device is not ready to receive recovery images");
            return assignPerformRecoveryError(
                "Device is not ready to receive recovery images");
        }

        auto [success, errorMsg] =
            recoveryCommands.performRecoveryCommand(imagePaths);
        if (!success)
        {
            logVerbose(errorMsg);
            return assignPerformRecoveryError(errorMsg);
        }
        logVerbose("Recovery Image Activated.");
        logVerbose("Perform Recovery Task Successful.");
        jsonResponse["Status"] = "Successful";
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        logVerbose("Error in Perform Recovery: " + std::string(e.what()));
        return assignPerformRecoveryError(std::string(e.what()));
    }
}

} // namespace recovery_tool
