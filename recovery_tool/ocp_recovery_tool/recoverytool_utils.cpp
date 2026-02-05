/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "recoverytool_utils.hpp"

#include <chrono>
#include <thread>

namespace recovery_tool
{

OCPRecoveryTool::OCPRecoveryTool(int busAddr, int slaveAddr, bool verb,
                                 bool emul) :
    verbose(verb), emul(emul), recoveryCommands(busAddr, slaveAddr, verb, emul)
{}

std::string OCPRecoveryTool::deviceIDToStr(DeviceId id) const noexcept
{
    switch (id)
    {
        case DeviceId::PCI_Vendor:
            return "PCI Vendor";
        case DeviceId::IANA:
            return "IANA";
        case DeviceId::UUID:
            return "UUID";
        case DeviceId::PnP_Vendor:
            return "PnP Vendor";
        case DeviceId::ACPI_Vendor:
            return "ACPI Vendor";
        case DeviceId::IANA_Enterprise_Type:
            return "IANA Enterprise Type";
        case DeviceId::NVMe_MI:
            return "NVMe MI";
        default:
            return "Reserved/Unknown";
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

nlohmann::json OCPRecoveryTool::getDeviceIDJson() noexcept
{
    nlohmann::json jsonResponse;

    try
    {
        logVerbose("Getting Device ID");
        auto [success, hexData, errorMsg] =
            recoveryCommands.getDeviceIDCommand();

        if (!success)
        {
            logVerbose("Error while getting Device Id: ", errorMsg);
            jsonResponse["Error"] = errorMsg;
            return jsonResponse;
        }

        const auto descriptorType = static_cast<DeviceId>(hexData[1]);
        if (descriptorType != DeviceId::PCI_Vendor)
        {
            jsonResponse["Error"] = "Found unknown Descriptor Type";
            return jsonResponse;
        }
        jsonResponse["Initial Descriptor Type"] = deviceIDToStr(descriptorType);
        jsonResponse["PCI Vendor ID"] = hexData[3] << 8 | hexData[4];
        jsonResponse["PCI DeviceId"] = hexData[5] << 8 | hexData[6];
        jsonResponse["PCI Subsystem Vendor ID"] = hexData[7] << 8 | hexData[8];
        jsonResponse["PCI Subsytem ID"] = hexData[9] << 8 | hexData[10];
        jsonResponse["PCI Revision ID"] = hexData[11];
    }
    catch (const std::exception& e)
    {
        try
        {
            logVerbose("Exception while getting Device ID: ", e.what());
            jsonResponse["Error"] = e.what();
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
        return jsonResponse;
    }

    return jsonResponse;
}

nlohmann::json OCPRecoveryTool::setForceRecoveryMode() noexcept
{
    nlohmann::json jsonResponse;

    try
    {
        logVerbose("Setting device into force recovery");
        const auto [setForceRecoveryStatus, errorMsg] =
            recoveryCommands.setForceRecoveryMode();
        if (setForceRecoveryStatus == true)
        {
            jsonResponse["Status"] = "Success";
        }
        else
        {
            jsonResponse["Error"] = errorMsg;
        }
    }
    catch (const std::exception& e)
    {
        try
        {
            logVerbose("Exception while setting force recovery mode: ",
                       e.what());
            jsonResponse["Error"] = e.what();
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
    }

    return jsonResponse;
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
                    std::stringstream ss;
                    ss << std::hex << std::uppercase << std::setw(2)
                       << std::setfill('0') << static_cast<int>(hexData[8 + i]);
                    vendorStatus += ss.str() + " ";
                }

                jsonResponse["Vendor Status(in hex)"] = vendorStatus;
            }
        }
        else
        {
            logVerbose("Error while getting Device Status: ", errorMsg);
            jsonResponse["Error"] = errorMsg;
        }
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        try
        {
            logVerbose("Error in GetDeviceStatus: ", e.what());
            jsonResponse["Error"] = e.what();
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
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
            // Extract recovery status from bits [3:0] of hexData[1]
            constexpr uint8_t recoveryStatusMask = 0x0F;
            uint8_t recoveryStatus = hexData[1] & recoveryStatusMask;

            // Extract recovery image index from bits [7:4] of hexData[1]
            constexpr uint8_t recoveryImageIndexMask = 0xF0;
            constexpr uint8_t recoveryImageIndexShift = 4;
            uint8_t recoveryImageIndex =
                (hexData[1] & recoveryImageIndexMask) >>
                recoveryImageIndexShift;

            jsonResponse["Device Recovery Status"] = recoveryStatusToStr(
                static_cast<RecoveryStatus>(recoveryStatus));
            jsonResponse["Recovery Image Index"] =
                std::to_string(recoveryImageIndex);
            jsonResponse["Vendor Specific Status"] = std::to_string(hexData[2]);
        }
        else
        {
            logVerbose("Error while getting Recovery Status: ", errMsg);
            jsonResponse["Error"] = errMsg;
        }
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        try
        {
            logVerbose("Error in GetRecoveryStatus: ", e.what());
            jsonResponse["Error"] = e.what();
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
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
        if (emul)
        {
            std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
        }

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
        if (emul)
        {
            std::this_thread::sleep_for(std::chrono::seconds(delay1sec));
        }

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
        try
        {
            logVerbose("Error in Perform Recovery: ", e.what());
            jsonResponse["Error"] = e.what();
            jsonResponse["Status"] = "Failed";
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
        return jsonResponse;
    }
}

nlohmann::json OCPRecoveryTool::processCMSLogs(const std::string& logFilePath,
                                               const uint8_t window)
{
    nlohmann::json jsonResponse;

    try
    {
        auto [success, hexData, errMsg] = recoveryCommands.getCMSLogs(window);

        if (success)
        {
            logVerbose("Writing " + logFilePath);
            auto [status, errorMsg] =
                recoveryCommands.saveToLogFile(hexData, logFilePath);
            if (!status)
            {
                jsonResponse["Status"] = "Failed";
                jsonResponse["Error"] = errorMsg;
            }
            else
            {
                jsonResponse["Status"] = "Successful";
            }
        }
        else
        {
            logVerbose("Error while getting CMS logs: " + errMsg);
            jsonResponse["Error"] = errMsg;
            jsonResponse["Status"] = "Failed";
        }
    }
    catch (const std::exception& e)
    {
        try
        {
            logVerbose("Exception while fetching CMS logs: ", e.what());
            jsonResponse["Error"] =
                std::string("Exception while fetching CMS logs: ") + e.what();
            jsonResponse["Status"] = "Failed";
        }
        catch (...)
        {
            // Prevent secondary exceptions from leaving the handler
        }
    }

    return jsonResponse;
}
} // namespace recovery_tool
