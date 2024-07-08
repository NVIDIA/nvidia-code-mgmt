#pragma once

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <string>

namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

using namespace phosphor::logging;

const std::string firmwareNotInRecovery{"NvidiaUpdate.1.0.FirmwareNotInRecovery"};
const std::string recoverySuccessful{"NvidiaUpdate.1.0.RecoverySuccessful"};
const std::string recoveryStarted{"NvidiaUpdate.1.0.RecoveryStarted"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

using DeviceName = std::string;
using ErrorCode = std::uint8_t;
using Message = std::string;
using Resolution = std::string;
using MessageMapping = std::pair<Message, Resolution>;
using ErrorMapping = std::unordered_map<ErrorCode, MessageMapping>;

ErrorCode constexpr deviceRecoveryFailed = 0x70;
ErrorCode constexpr deviceNotResponding = 0x71;
ErrorCode constexpr noDevicesFound = 0x72;

enum class RecoveryProtocol : uint8_t
{
    GlacierRecovery = 0x0,
    OCPRecoveryStatusError = 0x1,
    OCPRecoveryProtocolError = 0x2,
    OCPDeviceStatusCode = 0x3,
    OCPRecovery = 0x4,
};

using RecoveryErrorMapping = std::unordered_map<RecoveryProtocol, ErrorMapping>;

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
    BFFIMC =
        0x8, // Missing/corrupt boot loader (first mutable code) firmware image
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

enum class OCPDeviceStatusCode : int
{
    DeviceError = 0x2,
    FatalError = 0xF
};

enum class OCPRecoveryProtocolError : uint8_t
{
    UnsupportedWriteCommand = 0x1,
    UnsupportedParameter = 0x2,
    LengthWriteError = 0x3,
    CrcError = 0x4,
    DeviceNotResponding = 0x5,
    GeneralProtocolError = 0xFF
};

/**
 * @enum GalcierRecoveryCompletionCode
 * @brief Enumerates different possible results or status for the operations
 * used in Glacier recovery processes.
 */
enum class GalcierRecoveryCompletionCode : uint8_t
{
    Ok = 0x0,
    IllegalPayloadLength = 0x1,
    CRCFailure = 0x2,
    ResponseCRCCompFailure = 0x3,
    IllegalHeaderOffset = 0x4,
    IllegalKeyHashBlobOffset = 0x5,
    IllegalFWImageWriteAddress = 0x6,
    InvalidCommandSignature = 0x7,
    FirmwareNotInRecovery = 0x8,
    InitResponseByteMismatch = 0x9,
    BadResponse = 0xA,
    InvalidCommand = 0xB,
    Pending = 0xC,
    FailedToReadData = 0xD,
    InvalidRevision = 0xE,
    SRAMCmdFailed = 0xF,
    FileOpenFailure = 0x10,
    FailedToReadVendorDetails = 0x11,
    FailedToReadHeader = 0x12,
    FailedToReadKHB = 0x13,
    FailedToReadFWImage = 0x14,
    NoDevicesFound = 0xFF
};

/**
 * @brief Maps recovery-related error codes to their corresponding messages and
 * resolutions.
 */
static ErrorMapping glacierRecoveryErrorMapping{
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::IllegalPayloadLength),
     {"Payload length exceeds the allocated space",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::CRCFailure),
     {"CRC check failed on the command parameters",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::ResponseCRCCompFailure),
     {"CRC mismatch between the request and response",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::IllegalHeaderOffset),
     {"Header offset exceeds the maximum allowed size",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::IllegalKeyHashBlobOffset),
     {"Key Hash Blob offset exceeds the maximum allowed limit",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::IllegalFWImageWriteAddress),
     {"Firmware image address exceeds the maximum allocated size",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::InvalidCommandSignature),
     {"Command signature failed the authentication check",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::FirmwareNotInRecovery),
     {"Device is not in Recovery",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::InitResponseByteMismatch),
     {"Initial response byte does not match expected value",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::InvalidCommand),
     {"The command issued is invalid",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::Pending),
     {"Command timed out",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::FailedToReadData),
     {"Failed to read data from the device",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::SRAMCmdFailed),
     {"Recovery image authentication failed",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::FileOpenFailure),
     {"Unable to open the recovery image file",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         GalcierRecoveryCompletionCode::FailedToReadVendorDetails),
     {"Failed to read the vendor details",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::FailedToReadHeader),
     {"Failed to read the image header",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::FailedToReadKHB),
     {"Failed to read key hash blob",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(GalcierRecoveryCompletionCode::FailedToReadFWImage),
     {"Failed to read the firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {deviceNotResponding,
     {"Device is not responding",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {deviceRecoveryFailed,
     {"Recovery failed due to unknown error",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {noDevicesFound,
     {"No Devices found to recover",
      ""}},
};

static ErrorMapping ocpRecoveryErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFNF),
     {"No Boot Failure detected",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFGHWE),
     {"Generic hardware error",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFGSE),
     {"Generic hardware soft error - soft error may be recoverable",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFSTF),
     {"Self-test failure (e.g., RSA self test failure, FIPs self test failure,, etc.)",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFCD),
     {"Corrupted/missing critical data",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKMMC),
     {"Missing/corrupt key manifest",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKMAF),
     {"Authentication Failure on key manifest",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFKIAR),
     {"Anti-rollback failure on key manifest",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIMC),
     {"Missing/corrupt boot loader (first mutable code) firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIAF),
     {"Authentication failure on boot loader (1st mutable code) firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFFIAR),
     {"Anti-rollback failure boot loader (1st mutable code) firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFMC),
     {"Missing/corrupt main/management firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFAF),
     {"Authentication Failure main/management firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFMFAR),
     {"Anti-rollback Failure main/management firmware image",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFMC),
     {"Missing/corrupt recovery firmware",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFAF),
     {"Authentication Failure recovery firmware",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::BFRFAR),
     {"Anti-rollback Failure on recovery firmware",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryErrorCode::FR),
     {"Forced Recovery",
      "Check if firmware is in recovery, and try recovery again using the correct package."}}};

static ErrorMapping ocpRecoveryStatusErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryStatus::RecoveryFailed),
     {"Recovery Failed",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::RecoveryImgAuthFailed),
     {"Recovery Image Authentication Failed",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::ErrorEnteringRecoveryMode),
     {"Error Entering Recovery Mode",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryStatus::InvalidCms),
     {"Invalid CMS image provided",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
};

static ErrorMapping ocpDeviceStatusErrorMapping{
    {static_cast<ErrorCode>(OCPDeviceStatusCode::DeviceError),
     {"Device Error",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPDeviceStatusCode::FatalError),
     {"Fatal Error detected",
      "Try recovery again using the correct package."}},
};

static ErrorMapping ocpRecoveryProtocolErrorMapping{
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::UnsupportedWriteCommand),
     {"Unsupported Write Command",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::UnsupportedParameter),
     {"Unsupported Parameter",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::LengthWriteError),
     {"Length Write Error",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::CrcError),
     {"Crc Error",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::DeviceNotResponding),
     {"Device is not responding",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPRecoveryProtocolError::GeneralProtocolError),
     {"General Protocol Error. Error in communicating with device",
      "Check if firmware is in recovery, and try recovery again using the correct package."}},
    {noDevicesFound,
     {"No Devices found to recover",
      ""}},
};

static const RecoveryErrorMapping recoveryMappingTbl = {
    {RecoveryProtocol::GlacierRecovery, glacierRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecovery, ocpRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecoveryStatusError, ocpRecoveryStatusErrorMapping},
    {RecoveryProtocol::OCPDeviceStatusCode, ocpDeviceStatusErrorMapping},
    {RecoveryProtocol::OCPRecoveryProtocolError,
     ocpRecoveryProtocolErrorMapping},
};

class MessageRegistry
{
  public:
    MessageRegistry(sdbusplus::bus::bus& bus) : bus(bus)
    {}
    /**
     * @brief log message registry entry
     *
     * @param[in] messageID - redfish message
     * @param[in] deviceName - device name
     */
    void createMessageRegistry(const std::string& messageID,
                               const std::string& deviceName) const;

    /**
     * @brief Get the Message for firmware recovery message registry
     *
     * @param[in] recoveryProtocol - An enum of type RecoveryProtocol
     * @param[in] errorCode - error code
     * @return  A tuple containing an error and a resolution - if error code
     * mapping is present
     */
    std::optional<std::tuple<std::string, std::string>>
        getMessage(const RecoveryProtocol& recoveryProtocol,
                   const ErrorCode& errorCode) const;

    /**
     * @brief Create a Message Registry for Resource Event Errors
     *
     * @param[in] messageID - redfish message id
     * @param[in] recoveryProtocol - An enum of type RecoveryProtocol
     * @param[in] errorCode - recovery error code
     * @param[in] deviceName - device name
     */
    void createMessageRegistryResourceErrors(
        const std::string& messageID, const RecoveryProtocol& recoveryProtocol,
        const ErrorCode& errorCode, const std::string& deviceName) const;

  private:
    sdbusplus::bus::bus& bus;
    /**
     * @brief Create a Log entry
     *
     * @param[in] messageID
     * @param[in] addData
     * @param[in] level
     */
    void createLog(const std::string& messageID,
                   std::map<std::string, std::string>& addData,
                   Level& level) const;
};
