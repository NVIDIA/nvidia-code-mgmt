#include "glacier_recovery_utils.hpp"

namespace glacier_recovery_tool
{

GlacierRecoveryTool::GlacierRecoveryTool(int busAddr, int slaveAddr,
                                         bool verbose) :
    verbose(verbose),
    recoveryCommands(busAddr, slaveAddr, verbose)
{}

nlohmann::json GlacierRecoveryTool::getRecoveryStatusJson()
{
    nlohmann::json jsonResponse{};
    try
    {
        recoveryCommands.logVerbose("Getting Device Status");
        auto recResult = recoveryCommands.performInitialization();
        if (recResult != glacier_recovery_commands::RecoveryResult::Ok)
        {
            auto recResultStr = recoveryCommands.recoveryResultToStr(recResult);
            if (recResult ==
                glacier_recovery_commands::RecoveryResult::DeviceNotInRecovery)
            {
                jsonResponse["Status"] = recResultStr;
            }
            else
            {
                recoveryCommands.logVerbose(
                    "Error while getting Recovery Status: " + recResultStr);
                jsonResponse["Error"] = recResultStr;
            }
            return jsonResponse;
        }
        jsonResponse["Status"] = "Device is in Recovery";
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        recoveryCommands.logVerbose("Error in GetRecoveryStatus: " +
                                    std::string(e.what()));
        jsonResponse["Error"] = e.what();
        return jsonResponse;
    }
}

nlohmann::json GlacierRecoveryTool::getFirmwareInfoJson()
{
    nlohmann::json jsonResponse {};
    try
    {
        auto initRes = recoveryCommands.performInitialization();
        if (initRes != glacier_recovery_commands::RecoveryResult::Ok)
        {
            auto initErrMsg = recoveryCommands.recoveryResultToStr(initRes);
            recoveryCommands.logVerbose(
                "Error while getting Recovery Status: " + initErrMsg);
            jsonResponse["Error"] = initErrMsg;
            return jsonResponse;
        }
        auto [getFWInfoRes, hexData] =
            recoveryCommands.getFirmwareInfoCommand();

        if (getFWInfoRes != glacier_recovery_commands::RecoveryResult::Ok)
        {
            auto getFWInfoErrMsg =
                recoveryCommands.recoveryResultToStr(getFWInfoRes);
            recoveryCommands.logVerbose("Error while getting Firmware Info: " +
                                        getFWInfoErrMsg);
            jsonResponse["Error"] = getFWInfoErrMsg;
            return jsonResponse;
        }
        jsonResponse["Build Number"] = (hexData[8] << 8) | hexData[7];
        jsonResponse["Firmware ID"] = hexData[9];
        jsonResponse["Device ID"] = (hexData[11] << 8) | hexData[10];
        jsonResponse["Sub ID"] = hexData[12];
        jsonResponse["Revision ID"] = hexData[13];

        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        recoveryCommands.logVerbose("Error in GetFirmwareInfo: " +
                                    std::string(e.what()));
        jsonResponse["Error"] = e.what();
        return jsonResponse;
    }
}

nlohmann::json GlacierRecoveryTool::performRecovery(const std::string& imgPath)
{
    nlohmann::json jsonResponse {};
    try
    {
        recoveryCommands.logVerbose("Perform Glacier Recovery Task Started.");

        auto initRes = recoveryCommands.performInitialization();
        if (initRes != glacier_recovery_commands::RecoveryResult::Ok)
        {
            auto initErrMsg = recoveryCommands.recoveryResultToStr(initRes);
            recoveryCommands.logVerbose(
                "Error while performing Initialization: " + initErrMsg);
            jsonResponse["Error"] = initErrMsg;
            return jsonResponse;
        }

        auto recResult = recoveryCommands.performGlacierRecovery(imgPath);
        if (recResult != glacier_recovery_commands::RecoveryResult::Ok)
        {
            auto errorMsg = recoveryCommands.recoveryResultToStr(recResult);
            recoveryCommands.logVerbose(
                "Error while performing Glacier Recovery " + errorMsg);
            jsonResponse["Error"] = errorMsg;
            return jsonResponse;
        }
        recoveryCommands.logVerbose("Perform Recovery Task Successful.");
        jsonResponse["Status"] = "Recovery Successful";
        return jsonResponse;
    }
    catch (const std::exception& e)
    {
        recoveryCommands.logVerbose(
            "Error while performing Glacier Recovery: " +
            std::string(e.what()));
        jsonResponse["Error"] = std::string(e.what());
        return jsonResponse;
    }
}

} // namespace glacier_recovery_tool
