#pragma once

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <fstream>
#include <string>

namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

using namespace phosphor::logging;

const std::string firmwareNotInRecovery{"OpenBMC.0.4.FirmwareNotInRecovery"};
const std::string recoverySuccessful{"OpenBMC.0.4.RecoverySuccessful"};
const std::string recoveryStarted{"OpenBMC.0.4.RecoveryStarted"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

using DeviceName = std::string;
using ErrorCode = std::uint8_t;
using Message = std::string;
using Resolution = std::string;
using MessageMapping = std::pair<Message, Resolution>;
using ErrorMapping = std::unordered_map<ErrorCode, MessageMapping>;

enum class RecoveryProtocol : uint8_t
{
    GlacierRecovery = 0x0,
    OCPRecoveryStatusError = 0x1,
    OCPRecoveryProtocolError = 0x2,
    OCPRecovery = 0x3
};

using RecoveryErrorMapping =
    std::unordered_map<RecoveryProtocol, ErrorMapping>;


enum class GlacierRecoveryErrorCode : uint8_t
{
    IllegalPayloadLength = 0x1,
    CRCFailure = 0x2,
    ResCrc = 0x3,
    IllegalKHBOffset = 0x5,
    InvalidCommandSignature = 0x6,
    IllegalClk = 0x7,
    SameClk = 0x8,
    OtherError = 0x9
};

enum class OCPRecoveryErrorCode : uint8_t
{
    BFNF = 0x0,   // No Boot Failure detected
    BFGHWE = 0x1, // Generic hardware error
    BFGSE = 0x2,  // Generic hardware soft error - soft error may be recoverable
    BFSTF = 0x3,  // Self-test failure
    BFCD = 0x4,   // Corrupted/missing critical data
    BFKMMC = 0x5, // Missing/corrupt key manifest
    BFKMAF = 0x6, // Authentication Failure on key manifest
    BFKIAR = 0x7, // Anti-rollback failure on key manifest
    BFFIMC = 0x8, // Missing/corrupt boot loader (first mutable code) firmware image
    BFFIAF = 0x9,  // Authentication failure on boot loader (1st mutable code)
                   // firmware image
    BFFIAR = 0xA,  // Anti-rollback failure boot loader (1st mutable code)
                   // firmware image
    BFMFMC = 0xB,  // Missing/corrupt main/management firmware image
    BFMFAF = 0xC,  // Authentication Failure main/management firmware image
    BFMFAR = 0xD,  // Anti-rollback Failure main/management firmware image
    BFRFMC = 0xE,  // Missing/corrupt recovery firmware
    BFRFAF = 0xF,  // Authentication Failure recovery firmware
    BFRFAR = 0x10, // Anti-rollback Failure on recovery firmware
    FR = 0x11,     // Forced Recovery
    ReservedStart = 0x12,
    ReservedEnd = 0x7F,
    VendorUniqueStart = 0x80,
    VendorUniqueEnd = 0xFF
};

enum class OCPRecoveryStatus : uint8_t
{
    NotInRecoveryMode = 0x0,
    AwaitingRecoveryImg = 0x1,
    BootingRecoveryImg = 0x2,
    RecoverySuccess = 0x3,
    RecoveryFailed = 0xC,
    RecoveryImgAuthFailed = 0xD,
    ErrorEnteringRecoveryMode = 0xE,
    InvalidCms = 0xF
};

enum class OCPRecoveryProtocolError : uint8_t
{
    UnsupportedWriteCommand = 0x1,
    UnsupportedParameter = 0x2,
    LengthWriteError = 0x3,
    CrcError = 0x4,
    GeneralProtocolError = 0xFF
};

static ErrorMapping glacierRecoveryErrorMapping{
    {static_cast<ErrorCode>(GlacierRecoveryErrorCode::IllegalPayloadLength),
     {"IllegalPayloadLength", ""}},
    {static_cast<ErrorCode>(GlacierRecoveryErrorCode::ResCrc),
     {"CRC Validation failed", ""}},
    {static_cast<ErrorCode>(GlacierRecoveryErrorCode::OtherError),
     {"Command failed",
      "Ensure device is in crisis recovery mode, and try the recovery again"}},
};

static ErrorMapping ocpRecoveryErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFNF),
        {"No Boot Failure detected", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFGHWE),
        {"Generic hardware error", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFGSE),
        {"Generic hardware soft error - soft error may be recoverable", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFSTF),
        {"Self-test failure (e.g., RSA self test failure, FIPs self test failure,, etc.)", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFCD),
        {"Corrupted/missing critical data", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKMMC),
        {"Missing/corrupt key manifest", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKMAF),
        {"Authentication Failure on key manifest", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKIAR),
        {"Anti-rollback failure on key manifest", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIMC),
        {"Missing/corrupt boot loader (first mutable code) firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIAF),
        {"Authentication failure on boot loader (1st mutable code) firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIAR),
        {"Anti-rollback failure boot loader (1st mutable code) firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFMC),
        {"Missing/corrupt main/management firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFAF),
        {"Authentication Failure main/management firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFAR),
        {"Anti-rollback Failure main/management firmware image", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFMC),
        {"Missing/corrupt recovery firmware", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFAF),
        {"Authentication Failure recovery firmware", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFAR),
        {"Anti-rollback Failure on recovery firmware", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::FR),
        {"Forced Recovery", "Ensure device is in "
            "recovery mode, and try recovery again using the correct package."}}
};

static ErrorMapping ocpRecoveryStatusErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryStatus::RecoveryFailed),
     {"Recovery Failed", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::RecoveryImgAuthFailed),
     {"Recovery Image Authentication Failed", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::ErrorEnteringRecoveryMode),
     {"Error Entering Recovery Mode", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::InvalidCms),
     {"Invalid CMS image provided", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
};

static ErrorMapping ocpRecoveryProtocolErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::UnsupportedWriteCommand),
     {"Unsupported Write Command", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::UnsupportedParameter),
     {"Unsupported Parameter", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::LengthWriteError),
     {"Length Write Error", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::CrcError),
     {"Crc Error", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::GeneralProtocolError),
     {"General Protocol Error. Error in communicating with device", "Ensure device is in "
         "recovery mode, and try recovery again using the correct package."}},
};

static const RecoveryErrorMapping recoveryMappingTbl = {
    {RecoveryProtocol::GlacierRecovery, glacierRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecovery, ocpRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecoveryStatusError, ocpRecoveryStatusErrorMapping},
    {RecoveryProtocol::OCPRecoveryProtocolError, ocpRecoveryProtocolErrorMapping},
};

class MessageRegistry
{
  public:
    MessageRegistry(sdbusplus::bus::bus& bus) : bus(bus)
    {}
    /**
     * @brief log message registry entry
     *
     * @param[in] messageid - redfish message
     * @param[in] compname - component name
     * @param[in] compversion - component version
     *
     * @return void
     */
    void createMessageRegistry(const std::string& messageid,
                               const std::string& devicename) const;

    /**
     * @brief Get the Message for firmware recovery message registry
     *
     * @param[in] errorCode - error code
     * @param[in] deviceName - optional device name
     * @return error and resolution - if error code mapping is present
     */
    std::optional<std::tuple<std::string, std::string>>
        getMessage(const RecoveryProtocol& recoveryProtocol,
                   const ErrorCode& errorCode) const;

    /**
     * @brief Create a Message Registry for Resource Event Errors
     *
     * @param[in] messageID - redfish message id
     * @param[in] ComponentName - redfish
     * @param[in] errorCode - recovery error code
     * @param[in] deviceName - device name
     */
    void createMessageRegistryResourceErrors(
        const std::string& messageID, const RecoveryProtocol& recoveryProtocol,
        const ErrorCode& errorCode,
        const std::string& deviceName) const;

  private:
    sdbusplus::bus::bus& bus;
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
                   std::map<std::string, std::string>& addData, Level& level) const;
};
