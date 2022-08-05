#pragma once
#include "config.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <fstream>
#include <sstream>

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
using namespace phosphor::logging;

// File type for debug token is 2 and file type for token request is 1
constexpr uint8_t FileTypeDebugToken = 2;

/**
 * @brief structure for debug token header
 *
 */
struct DebugTokenHeader
{
    uint8_t version;
    uint8_t type;
    uint16_t numberOfRecords;
    uint16_t offsetToListOfStructs;
    uint32_t fileSize;
    uint8_t reserved[6];
} __attribute__((packed));

/**
 * @brief structure for each debug token
 *
 */
struct DebugToken
{
    char identifier[4];
    uint32_t version;
    uint16_t structSize;
    uint16_t tokenAttributes;
    uint32_t tokenType;
    uint32_t ecFWVersion;
    uint8_t noOnce[16];
    uint8_t serialNumber[8];
    uint8_t reserved[20];
    uint8_t publicKey[96];
    uint8_t signature[96];
} __attribute__((packed));

struct TokenUtility
{
    /**
     * @brief Get the Debug Token Header from debug token file
     *
     * @param[in] headerData
     * @param[in] debugTokenPackage
     *
     * @return DebugTokenHeader
     */
    auto getDebugTokenHeader(std::vector<uint8_t>& headerData,
                            std::ifstream& debugTokenPackage)
    {
        const DebugTokenHeader* headerInfo = nullptr;
        debugTokenPackage.seekg(0);
        debugTokenPackage.read(reinterpret_cast<char*>(headerData.data()),
                            sizeof(DebugTokenHeader));
        headerInfo = reinterpret_cast<const DebugTokenHeader*>(headerData.data());
        if (headerInfo->type != FileTypeDebugToken)
        {
            headerInfo = nullptr;
        }
        return headerInfo;
    }

    /**
     * @brief get next debug token from debug token file
     *
     * @param[in] tokenData
     * @param[in] tokenOffset
     * @param[in] debugTokenPackage
     *
     * @return DebugToken
     */
    auto gextNextDebugToken(std::vector<uint8_t>& tokenData,
                            const uint32_t& tokenOffset,
                            std::ifstream& debugTokenPackage)
    {
        const DebugToken* debugTokenInfo = nullptr;
        debugTokenPackage.seekg(tokenOffset);
        debugTokenPackage.read(reinterpret_cast<char*>(tokenData.data()),
                            sizeof(DebugToken));
        debugTokenInfo = reinterpret_cast<const DebugToken*>(tokenData.data());
        return debugTokenInfo;
    }

    /**
     * @brief execute mctp vdm util command and return status code and command
     * output
     *
     * @param[in] command - vdm util command to execute
     *
     * @return std::pair<int, std::string> - status, command output
     */
    std::pair<int, std::string> runMctpVdmUtilCommand(const std::string& command)
    {
        std::array<char, 1024> buffer;
        std::stringstream commandOut;
        int retCode;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
        {
            log<level::ERR>("popen() failed!");
            retCode = -1;
        }
        else
        {
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
            {
                commandOut << buffer.data();
            }
            retCode = pclose(pipe);
        }
        return {retCode, commandOut.str()};
    }

    /**
     * @brief parse mctp-vdm command output, it will extract RX bytes from the
             response
    *         Example:
    *          Test command = debug_token_erase
    *          teid = 24
    *          TX: 47 16 00 00 80 01 0C 01
    *          RX: 47 16 00 00 00 01 0C 01 00 00
    *
    * @param[in] output - command output to parse
    *
    * @return rxBytes - response bytes
    */
    std::vector<std::string> parseCommandOutput(const std::string& output)
    {
        std::vector<std::string> rxBytes;
        std::vector<std::string> lines;
        boost::split(lines, output, boost::is_any_of("\n"));
        for (std::string& line : lines)
        {
            if (line.find("RX: ") == 0)
            {
                boost::algorithm::trim(line);
                boost::replace_all(line, "RX: ", "");
                boost::split(rxBytes, line, boost::is_any_of(" "));
            }
        }
        return rxBytes;
    }
};
