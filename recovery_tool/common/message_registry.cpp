
#include "message_registry.hpp"

#include <phosphor-logging/lg2.hpp>

void MessageRegistry::createLog(const std::string& messageID,
                                std::map<std::string, std::string>& addData,
                                Level& level) const
{
    static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
    static constexpr auto logInterface = "xyz.openbmc_project.Logging.Create";
    static constexpr auto service = "xyz.openbmc_project.Logging";
    try
    {
        auto severity = LoggingServer::convertForMessage(level);
        auto method =
            bus.new_method_call(service, logObjPath, logInterface, "Create");
        method.append(messageID, severity, addData);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create D-Bus log entry for message registry",
                   "ERROR", e.what());
    }
    return;
}

void MessageRegistry::createMessageRegistry(const std::string& messageID,
                                            const std::string& deviceName) const
{
    std::map<std::string, std::string> addData;
    Level level = Level::Informational;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    if ((messageID == recoveryStarted) ||
        (messageID == firmwareNotInRecovery) ||
        (messageID == recoverySuccessful) || (messageID == enterDOTRecovery))
    {
        addData["REDFISH_MESSAGE_ARGS"] = deviceName;
    }
    else
    {
        lg2::error("Message Registry messageID = {MESSAGEID} is not recognised",
                   "MESSAGEID", messageID);
        return;
    }
    addData["namespace"] = "FWUpdate";
    createLog(messageID, addData, level);
    return;
}

std::optional<std::tuple<std::string, std::string>>
    MessageRegistry::getMessage(const RecoveryProtocol& recoveryProtocol,
                                const ErrorCode& errorCode) const
{
    Message errorMessage;
    Resolution resolution;
    if (recoveryMappingTbl.contains(recoveryProtocol))
    {
        auto recoveryMapping = recoveryMappingTbl.find(recoveryProtocol);
        if (recoveryMapping->second.contains(errorCode))
        {
            auto errorCodeSearch = recoveryMapping->second.find(errorCode);
            errorMessage = errorCodeSearch->second.first;
            resolution = errorCodeSearch->second.second;
            return {{errorMessage, resolution}};
        }
        else
        {
            lg2::error(
                "Error Code: {ERRORCODE} not found for recovery protocol: {RECOVERYPROTOCOL}",
                "ERRORCODE", unsigned(errorCode), "RECOVERYPROTOCOL",
                unsigned(recoveryProtocol));
        }
    }
    else
    {
        lg2::error(
            "No error code mapping found for recovery protocol : {RECOVERYPROTOCOL}",
            "RECOVERYPROTOCOL", unsigned(recoveryProtocol));
    }
    return {};
}

void MessageRegistry::createMessageRegistryResourceErrors(
    const std::string& messageID, const RecoveryProtocol& recoveryProtocol,
    const ErrorCode& errorCode, const std::string& deviceName,
    Level severity) const
{
    std::optional<std::tuple<std::string, std::string>> message =
        getMessage(recoveryProtocol, errorCode);
    if (message)
    {
        std::map<std::string, std::string> addData;
        Level level = severity;
        addData["REDFISH_MESSAGE_ID"] = messageID;
        addData["REDFISH_MESSAGE_ARGS"] =
            (deviceName + "," + std::get<0>(*message));
        // use separate container for fwupdate message registry
        addData["namespace"] = "FWUpdate";
        std::string resolution = std::get<1>(*message);
        if (!resolution.empty())
        {
            addData["xyz.openbmc_project.Logging.Entry.Resolution"] =
                resolution;
        }
        createLog(messageID, addData, level);
    }
    else
    {
        log<level::ERR>("Unable to log message registry.",
                        entry("DeviceName: %s", deviceName.c_str()));
    }
    return;
}
