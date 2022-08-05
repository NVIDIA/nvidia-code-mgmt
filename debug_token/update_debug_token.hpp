#pragma once
#include "config.h"

#include "token_utility.hpp"

#include <fstream>
#include <map>

namespace dbus
{

using ObjectPath = std::string;
using Interface = std::string;
using Interfaces = std::vector<std::string>;
using Property = std::string;
using PropertyType = std::string;
using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;

using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
} // namespace dbus

using UUID = std::string;
using EID = size_t;
using SupportedMessageTypes = std::vector<uint8_t>;
using MctpInfo = std::map<UUID, EID>;
using SerialNumber = std::string;
using Token = std::vector<uint8_t>;
using DeviceMap = std::map<EID, SerialNumber>;
using TokenMap = std::map<SerialNumber, Token>;
using MctpMedium = std::string;
using namespace dbus;
namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

static constexpr uint8_t mctpTypeSPDM = 0x5;
constexpr auto mctpService = "xyz.openbmc_project.MCTP.Control.PCIe";
constexpr auto mctpPath = "/xyz/openbmc_project/mctp";
constexpr auto mctpEndpointIntfName = "xyz.openbmc_project.MCTP.Endpoint";
constexpr auto uuidEndpointIntfName = "xyz.openbmc_project.Common.UUID";
constexpr auto pldmService = "xyz.openbmc_project.PLDM";
constexpr auto pldmPath = "/";
constexpr auto pldmInventoryIntfName =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
const std::string mctpVdmUtilPath = "/usr/bin/mctp-vdm-util";
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};

/* Debug token install command error codes */
enum InstallErrorCodes
{
    InstallSuccess = 0x0,
    InvalidToken,
    TokenAuthFailed,
    TokenNonceInvalid,
    TokenSerialNumberInvalid,
    TokenECFWVersionInvalid,
    DisableBackgroundCopyCheckFailed,
    InstallInternalError
};

/* Debug token erase command error codes */
enum EraseErrorCodes
{
    EraseSuccess = 0x0,
    EraseInternalError = 0x1
};

/* background copy enabled or disable command error codes */
enum BackgroundCopyErrorCodes
{
    BackgroundCopySuccess = 0x0,
    BackgroundCopyFailed = 0x1
};

/**
 * @brief implemementation of update debug token
 *
 */
class UpdateDebugToken : public TokenUtility
{
  public:
    /**
     * @brief Construct a new Debug token ItemUpdater object
     *
     * @param[in] bus
     */
    UpdateDebugToken(sdbusplus::bus::bus& bus) : bus(bus)
    {}
    /**
     * @brief install debug token for all matching devices
     *
     * @param[in] debugTokenPath
     * 
     * @return int
     */
    int installDebugToken(const std::string& debugTokenPath);
    /**
     * @brief erase debug token for all discovered devices
     *
     * @return int
     */
    int eraseDebugToken();
    /**
     * @brief update token map which has mapping of serial number to EID
     *
     * @param[in] debugTokenPath - debug token file path
     * @param[in] tokens - token map
     * 
     * @return int
     */
    int updateTokenMap(const std::string& debugTokenPath, TokenMap& tokens);
    /**
     * @brief log message registry entry
     *
     * @param[in] messageID - redfish message
     * @param[in] compName - component name
     * @param[in] compVersion - component version
     * 
     * @return void
     */
    void createMessageRegistry(const std::string& messageID,
                               const std::string& compName,
                               const std::string& compVersion);

  private:
    sdbusplus::bus::bus& bus;
    /* device map of EID to serial number */
    DeviceMap devices;
    /* map of UUID to EID */
    MctpInfo mctpInfo;
    /**
     * @brief discover MCTP end points
     *
     * @return int - status code
     */
    int discoverMCTPDevices();
    /**
     * @brief update device map of eid->serial number
     *
     * @param[in] interfaces
     */
    void updateDeviceMap(const dbus::InterfaceMap& interfaces);
    /**
     * @brief update end points, discover MCTP end points, get serial number
     * from pldm D-Bus object and create a map of serial number to EID
     *
     * @return int
     */
    int updateEndPoints();
    /**
     * @brief enable background copy
     *
     * @param[in] eid
     * 
     * @return int
     */
    int enableBackgroundCopy(const EID& eid);
    /**
     * @brief disable background copy
     *
     * @param[in] eid
     * 
     * @return int
     */
    int disableBackgroundCopy(const EID& eid);
    /**
     * @brief install token on the device
     *
     * @param[in] eid
     * @param[in] token
     * 
     * @return int
     */
    int installToken(const EID& eid, const Token& token);
    /**
     * @brief erase token on the device
     *
     * @param[in] eid
     * 
     * @return int
     */
    int eraseToken(const EID& eid);
    /**
     * @brief Create a Log entry
     *
     * @param[in] messageID
     * @param[in] addData
     * @param[in] level
     * 
     * @return void
     */
    void createLog(const std::string& messageID,
                   std::map<std::string, std::string>& addData, Level& level);
};
