#include "config.h"

#include "update_debug_token.hpp"

#include <filesystem>

int UpdateDebugToken::installDebugToken(const std::string& debugTokenPath)
{
    int status = 0;
    TokenMap tokens;
    if (updateEndPoints() != 0)
    {
        log<level::ERR>("discovery failed");
        status = -1;
        return status;
    }
    if (updateTokenMap(debugTokenPath, tokens) != 0)
    {
        log<level::ERR>("Error while parsing tokens");
        status = -1;
        return status;
    }
    for (auto& device : devices)
    {
        if (tokens.find(device.second) != tokens.end())
        {
            if (queryDebugToken(device.first) != DebugTokenNotInstalled)
            {
                log<level::ERR>(("debug token already installed on EID " +
                                 std::to_string(device.first))
                                    .c_str());
                // skip install token for this device since token is already
                // installed
                continue;
            }
            if (disableBackgroundCopy(device.first) != 0)
            {
                log<level::ERR>(("Disable BackgroundCopy failed for EID " +
                                 std::to_string(device.first))
                                    .c_str());
                status = -1;
                // abort install token for this device if disabling background
                // copy is failed
                continue;
            }
            else
            {
                log<level::INFO>(("Disable BackgroundCopy success for EID " +
                                  std::to_string(device.first))
                                     .c_str());
            }
            if (installToken(device.first, tokens[device.second]) != 0)
            {
                log<level::ERR>(("DebugToken Install failed for EID " +
                                 std::to_string(device.first))
                                    .c_str());
                status = -1;
            }
            else
            {
                log<level::INFO>(("DebugToken Install success for EID " +
                                  std::to_string(device.first))
                                     .c_str());
            }
        }
    }
    return status;
}

int UpdateDebugToken::eraseDebugToken()
{
    int status = 0;
    if (discoverMCTPDevices() != 0)
    {
        log<level::ERR>("Error while discovering MCTP devices");
        status = -1;
    }
    for (auto& device : mctpInfo)
    {
        if (queryDebugToken(device.second) == DebugTokenNotInstalled)
        {
            // skip erase token for this device since token is not installed
            continue;
        }
        if (enableBackgroundCopy(device.second) != 0)
        {
            log<level::ERR>(("Enable BackgroundCopy failed for EID " +
                             std::to_string(device.second))
                                .c_str());
            status = -1;
            // proceed with erase token if enabling background copy has failed
        }
        else
        {
            log<level::INFO>(("Enable BackgroundCopy success for EID " +
                              std::to_string(device.second))
                                 .c_str());
        }
        if (eraseToken(device.second) != 0)
        {
            log<level::ERR>(("DebugToken Erase failed for EID=" +
                             std::to_string(device.second))
                                .c_str());
            status = -1;
        }
        else
        {
            log<level::INFO>(("DebugToken Erase success for EID=" +
                              std::to_string(device.second))
                                 .c_str());
        }
    }
    return status;
}

int UpdateDebugToken::discoverMCTPDevices()
{
    int status = 0;
    dbus::ObjectValueTree objects{};
    try
    {
        auto method = bus.new_method_call(mctpService, mctpPath,
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects");
        auto reply = bus.call(method);
        reply.read(objects);
        for (const auto& [objectPath, interfaces] : objects)
        {
            UUID uuid{};
            if (interfaces.contains(uuidEndpointIntfName))
            {
                const auto& properties = interfaces.at(uuidEndpointIntfName);
                if (properties.contains("UUID"))
                {
                    uuid = std::get<UUID>(properties.at("UUID"));
                }
            }
            if (uuid.empty())
            {
                continue;
            }
            if (interfaces.contains(mctpEndpointIntfName))
            {
                const auto& properties = interfaces.at(mctpEndpointIntfName);
                if (properties.contains("EID") &&
                    properties.contains("SupportedMessageTypes"))
                {
                    auto mctpTypes = std::get<SupportedMessageTypes>(
                        properties.at("SupportedMessageTypes"));
                    if (std::find(mctpTypes.begin(), mctpTypes.end(),
                                  mctpTypeSPDM) != mctpTypes.end())
                    {
                        auto eid = std::get<EID>(properties.at("EID"));
                        mctpInfo.emplace(uuid, eid);
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("D-Bus error", entry("ERROR=%s", e.what()));
        status = -1;
    }
    return status;
}

void UpdateDebugToken::updateDeviceMap(const dbus::InterfaceMap& interfaces)
{
    UUID uuid{};
    if (interfaces.contains(uuidEndpointIntfName))
    {
        const auto& properties = interfaces.at(uuidEndpointIntfName);
        if (properties.contains("UUID"))
        {
            uuid = std::get<std::string>(properties.at("UUID"));
        }
    }
    if (uuid.empty())
    {
        return;
    }
    if (interfaces.contains(pldmInventoryIntfName))
    {
        const auto& properties = interfaces.at(pldmInventoryIntfName);
        if (properties.contains("SerialNumber"))
        {
            SerialNumber serialNumber =
                std::get<SerialNumber>(properties.at("SerialNumber"));
            if (mctpInfo.find(uuid) != mctpInfo.end())
            {
                devices.emplace(mctpInfo[uuid], serialNumber);
            }
        }
    }
}

int UpdateDebugToken::updateEndPoints()
{
    int status = 0;
    dbus::ObjectValueTree objects{};

    if (discoverMCTPDevices() != 0)
    {
        log<level::ERR>("Error while discovering MCTP devices");
        status = -1;
        return status;
    }
    try
    {
        auto method = bus.new_method_call(pldmService, pldmPath,
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects");
        auto reply = bus.call(method);
        reply.read(objects);
        for (const auto& [objectPath, interfaces] : objects)
        {
            updateDeviceMap(interfaces);
        }
    }
    catch (const std::exception& e)
    {
        status = -1;
        log<level::ERR>("D-Bus error", entry("ERROR=%s", e.what()));
        return status;
    }
    return status;
}

int UpdateDebugToken::updateTokenMap(const std::string& debugTokenPath,
                                     TokenMap& tokens)
{
    int status = 0;
    std::ifstream debugTokenPackage(
        debugTokenPath, std::ios::binary | std::ios::in | std::ios::ate);
    uint32_t packageSize = debugTokenPackage.tellg();
    if (!debugTokenPackage.is_open())
    {
        log<level::ERR>("Error while opening the file");
        status = -1;
        return status;
    }
    std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
    auto headerInfo = getDebugTokenHeader(headerData, debugTokenPackage);
    if (!headerInfo)
    {
        log<level::ERR>("Invalid token header");
        status = -1;
        return status;
    }
    uint32_t tokenOffset = headerInfo->offsetToListOfStructs;
    for (uint16_t i = 0; i < headerInfo->numberOfRecords; i++)
    {
        Token token(sizeof(DebugToken));
        if ((tokenOffset + sizeof(DebugToken)) > packageSize)
        {
            log<level::ERR>("Token offset out of range");
            break;
        }
        auto debugTokenInfo =
            gextNextDebugToken(token, tokenOffset, debugTokenPackage);
        if (debugTokenInfo)
        {
            std::stringstream serialNumber;
            serialNumber << std::hex;
            for (size_t x = 0; x < sizeof(debugTokenInfo->serialNumber); x++)
            {
                serialNumber << std::uppercase << std::setw(2)
                             << std::setfill('0')
                             << (int)debugTokenInfo->serialNumber[x];
            }
            tokens.emplace(("0x" + serialNumber.str()), token);
        }
        else
        {
            log<level::ERR>(
                "Invalid debug token"); // skip and move to next token
        }
        tokenOffset += sizeof(DebugToken);
    }
    return status;
}

int UpdateDebugToken::installToken(const EID& eid, const Token& token)
{
    int status = 0;
    std::string command = "";
    std::stringstream tokenString;
    tokenString << std::hex;
    for (size_t x = 0; x < token.size(); x++)
    {
        tokenString << std::setw(2) << std::setfill('0') << (int)token[x]
                    << " ";
    }
    // form install command
    command += mctpVdmUtilPath;
    command += " -c debug_token_install ";
    command += "-t " + std::to_string(eid);
    command += " " + tokenString.str();
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>("Error while running install token command");
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = InstallInternalError;
        log<level::ERR>("Error while getting status code");
    }
    if (status != InstallSuccess)
    {
        log<level::ERR>(
            ("Error while installing token: " + commandOut).c_str());
        status = -1;
        // enable background copy since token installation is failed
        // which is the default setting
        if (enableBackgroundCopy(eid) != 0)
        {
            log<level::ERR>(
                ("Enable BackgroundCopy failed for EID " + std::to_string(eid))
                    .c_str());
        }
        else
        {
            log<level::INFO>(
                ("Enable BackgroundCopy success for EID " + std::to_string(eid))
                    .c_str());
        }
    }
    return status;
}

int UpdateDebugToken::eraseToken(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c debug_token_erase ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>("Error while running erase token command");
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = EraseInternalError;
        log<level::ERR>("Error while getting status code");
    }
    if (status != EraseSuccess)
    {
        log<level::ERR>(("Error while erasing token: " + commandOut).c_str());
        status = -1;
        // disable background copy since token erase is failed
        // this will avoid debug image getting copied to both the partition
        if (disableBackgroundCopy(eid) != 0)
        {
            log<level::ERR>(
                ("Disable BackgroundCopy failed for EID " + std::to_string(eid))
                    .c_str());
        }
        else
        {
            log<level::INFO>(("Disable BackgroundCopy success for EID " +
                              std::to_string(eid))
                                 .c_str());
        }
    }
    return status;
}

int UpdateDebugToken::disableBackgroundCopy(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c background_copy_disable ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        status = -1;
        log<level::ERR>("Error while running background copy disable command");
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = BackgroundCopyFailed;
        log<level::ERR>("Error while getting status code");
    }
    if (status != BackgroundCopySuccess)
    {
        log<level::ERR>(
            ("Error while disabling background copy: " + commandOut).c_str());
        status = -1;
    }
    return status;
}

int UpdateDebugToken::enableBackgroundCopy(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c background_copy_enable ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>("Error while running background copy enable command");
        status = -1;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // last byte is status code
        status = std::stoi(rxBytes[rxBytes.size() - 1], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = BackgroundCopyFailed;
        log<level::ERR>("Error while getting status code");
    }
    if (status != BackgroundCopySuccess)
    {
        log<level::ERR>(
            ("Error while enabling background copy: " + commandOut).c_str());
        status = -1;
    }
    return status;
}

int UpdateDebugToken::queryDebugToken(const EID& eid)
{
    int status = 0;
    std::string command = "";
    // form erase command
    command += mctpVdmUtilPath;
    command += " -c debug_token_query ";
    command += "-t " + std::to_string(eid);
    auto [retCode, commandOut] = runMctpVdmUtilCommand(command);
    if (retCode != 0)
    {
        log<level::ERR>("Error while running debug token query command");
        status = DebugTokenNotInstalled;
        return status;
    }
    auto rxBytes = parseCommandOutput(commandOut);
    try
    {
        // 11 the byte from last is status code
        status = std::stoi(rxBytes[rxBytes.size() - 11], nullptr, 16);
    }
    catch (const std::exception& e)
    {
        status = DebugTokenNotInstalled;
        log<level::ERR>("Error while getting status code");
    }
    if (status != 0)
    {
        log<level::ERR>(
            ("Error while parsing debug token query output: " + commandOut)
                .c_str());
        status = DebugTokenNotInstalled;
        return status;
    }
    try
    {
        // 10 the byte from last is token installation status
        auto tokenInstallStatus =
            std::stoi(rxBytes[rxBytes.size() - 10], nullptr, 16);
        if (tokenInstallStatus == DebugTokenInstalled)
        {
            status = DebugTokenInstalled;
        }
        else
        {
            status = DebugTokenNotInstalled;
        }
    }
    catch (const std::exception& e)
    {
        status = DebugTokenNotInstalled;
        log<level::ERR>("Error while getting token installation status");
    }
    return status;
}

void UpdateDebugToken::createLog(const std::string& messageID,
                                 std::map<std::string, std::string>& addData,
                                 Level& level)
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
        log<level::ERR>("Failed to create D-Bus log entry for message registry",
                        entry("ERROR=%s", e.what()));
    }
    return;
}

void UpdateDebugToken::createMessageRegistry(const std::string& messageID,
                                             const std::string& compName,
                                             const std::string& compVersion)
{
    std::map<std::string, std::string> addData;
    Level level = Level::Informational;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    if (messageID == updateSuccessful)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compName + "," + compVersion);
    }
    else
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compVersion + "," + compName);
        level = Level::Critical;
    }
    // use separate container for fwupdate message registry
    addData["namespace"] = "FWUpdate";
    createLog(messageID, addData, level);
    return;
}