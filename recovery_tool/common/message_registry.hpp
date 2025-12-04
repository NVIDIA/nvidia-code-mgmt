#pragma once

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

#include <string>

namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

using namespace phosphor::logging;

const std::string firmwareNotInRecovery{
    "NvidiaUpdate.1.0.FirmwareNotInRecovery"};
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
    MCURecovery = 0x5,
    USBRCMRecovery = 0x6
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

enum class MCURecoveryErrorCode : uint8_t
{
    CmdExecFailed = 0x1,
    DevNotProvisioned = 0x2,
    GetSecurityStateFailed = 0x3,
    NotSecureDevice = 0x4,
    ReadMemoryFailed = 0x5,
    EncryptKeyNotSet = 0x6,
    SbFileWriteFailed = 0x7,
    InvalidSB3File = 0x8,
};

enum class USBRCMRecoveryErrorCode : uint8_t
{
    // General USB RCM recovery errors (0x01-0x1F)
    DeviceNotFound = 0x01,
    InitializationFailed = 0x02,
    ForceRecoveryFailed = 0x03,
    ImageTransferFailed = 0x04,
    RecoveryVerificationFailed = 0x05,
    DeviceNotInRecovery = 0x08,
    Timeout = 0x0C,
    FailedToReadData = 0x0D,
    FileOpenFailure = 0x10,
    InvalidImageOrder = 0x11,
    USBCommunicationError = 0x12,
    DOTBlobRequired = 0x13,
    S2ABlobNotSupported = 0x14,

    // PSC ROM Error Codes (0x20-0x4F)
    PscRomI2cExtMsgFail = 0x21,
    PscRomBootModeSelFail = 0x22,
    PscRomUsbExtMsgFail = 0x23,
    PscRomErotGrantFail = 0x24,
    PscRomQspi0DevFail = 0x25,
    PscRomUsb2DevFail = 0x26,
    PscRomOcprcDevFail = 0x27,
    PscRomBootChainExhaust = 0x28,
    PscRomBootImageLoadFail = 0x29,
    PscRomDotSlotExhaust = 0x2A,
    PscRomS2aHeaderCheckFail = 0x2B,
    PscRomS2aSanityFail = 0x2C,
    PscRomS2aIntegrityFail = 0x2D,
    PscRomS2aKeyRevoked = 0x2E,
    PscRomMutableDotHeaderCheckFail = 0x2F,
    PscRomMutableDotIntegrityFail = 0x30,
    PscRomMutableDotSanityFail = 0x31,
    PscRomVolatileDotParityFail = 0x32,
    PscRomVolatileDotHeaderCheckFail = 0x33,
    PscRomVolatileDotSanityFail = 0x34,
    PscRomCaliptraAckFail = 0x35,
    PscRomFmcCshSanityFail = 0x36,
    PscRomFmcCshAuthz1Fail = 0x37,
    PscRomFmcCshAuthz2Fail = 0x38,
    PscRomFmcCshBinListIntegrityFail = 0x39,
    PscRomFmcImageSanityFail = 0x3A,
    PscRomFmcImageIntegrityFail = 0x3B,
    PscRomFmcImageDecryptionFail = 0x3C,
    PscRomFmcImageOemRatchetFail = 0x3D,
    PscRomFmcImageMutDotSvnFuseRatchetFail = 0x3E,
    PscRomFmcImageMutDotSvnCshRatchetFail = 0x3F,
    PscRomFmcImageVolDotSvnFuseRatchetFail = 0x40,
    PscRomFmcImageVolDotSvnCshRatchetFail = 0x41,
    PscRomFmcImageVolDotCshRatchetFail = 0x42,

    // PSC FMC Error Codes (0x50-0x6F)
    PscFmcFuseCrcFailed = 0x51,
    PscFmcLpiLsLinkFailed = 0x52,
    PscFmcBootChainLedgerInvalid = 0x53,
    PscFmcBootChainLedgerNotBootable = 0x54,
    PscFmcBootChainLedgerMismatch = 0x55,
    PscFmcDebugTokenSanityFail = 0x56,
    PscFmcDebugTokenAuthenticationFail = 0x57,
    PscFmcSanityFailed = 0x58,
    PscFmcStage1AuthenticationFailed = 0x59,
    PscFmcStage2AuthenticationFailed = 0x5A,
    PscFmcSvnCheckFailed = 0x5B,
    PscFmcHaltDisabledSocket = 0x5C,
    PscFmcMemFuseCrcFailed = 0x5D,
    PscFmcCaliptraMailboxFailed = 0x5E,
    PscFmcCsaFailed = 0x5F,
    PscFmcDotFailed = 0x60,
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
    {noDevicesFound, {"No Devices found to recover", ""}},
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
     {"Device Error", "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(OCPDeviceStatusCode::FatalError),
     {"Fatal Error detected", "Try recovery again using the correct package."}},
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
    {noDevicesFound, {"No Devices found to recover", ""}},
};

static ErrorMapping mcuRecoveryErrorMapping{
    {static_cast<ErrorCode>(MCURecoveryErrorCode::CmdExecFailed),
     {"blhost command execution failed",
      "Check if device is in ISP mode (recovery mode), and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::DevNotProvisioned),
     {"Device is not provisioned", "Return to NVIDIA for provisioning."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::GetSecurityStateFailed),
     {"blhost get-property security-state command execution failed",
      "Check if device is in ISP mode (recovery mode), and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::NotSecureDevice),
     {"Device is not fully provisioned (MCU is not locked)",
      "Return to NVIDIA for provisioning."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::ReadMemoryFailed),
     {"blhost read-memory command execution failed",
      "Check if device is in ISP mode (recovery mode), and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::EncryptKeyNotSet),
     {"Encrypt key is not set", "Return to NVIDIA for provisioning."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::SbFileWriteFailed),
     {"blhost receive-sb-file command execution failed",
      "Check if device is in ISP mode (recovery mode) or if the SB file is invalid, and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(MCURecoveryErrorCode::InvalidSB3File),
     {"SB3 file header mismatch",
      "Check if the SB3 file in fwpkg is valid, and try recovery again using the correct package."}},
    {deviceNotResponding,
     {"Device is not responding",
      "Check if device is in ISP mode (recovery mode) , and try recovery again using the correct package."}},
    {deviceRecoveryFailed,
     {"Recovery failed due to unknown error",
      "Check if device is in ISP mode (recovery mode), and try recovery again using the correct package."}},
    {noDevicesFound, {"No Devices found to recover", ""}},
};

static ErrorMapping usbRcmRecoveryErrorMapping{
    // General USB RCM recovery errors
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::DeviceNotFound),
     {"Device not found at specified USB port",
      "Check USB connection and verify device is connected at the correct port."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::InitializationFailed),
     {"Failed to initialize USB subsystem",
      "Check USB permissions and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::ForceRecoveryFailed),
     {"Failed to set device into force recovery mode",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::ImageTransferFailed),
     {"Failed to transfer recovery images to device",
      "Check image files are valid and try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::RecoveryVerificationFailed),
     {"Recovery verification failed",
      "Check device boot status and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::DeviceNotInRecovery),
     {"Device is not in recovery mode",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::Timeout),
     {"Timeout waiting for recovery completion",
      "Check device boot progress and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::FailedToReadData),
     {"Failed to read data from device",
      "Check USB connection and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::FileOpenFailure),
     {"Unable to open recovery image file",
      "Check image file paths and permissions, and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::InvalidImageOrder),
     {"Invalid image order or missing required images",
      "Check all 9 recovery images are present in the package and try again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::USBCommunicationError),
     {"USB communication error",
      "Check USB connection and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::DOTBlobRequired),
     {"DOT blob required but not provided",
      "Ensure the DOT blob file is included in the recovery package."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::S2ABlobNotSupported),
     {"S2A blob required but not supported",
      "Contact NVIDIA support for S2A blob recovery assistance."}},

    // PSC ROM Error Codes
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomI2cExtMsgFail),
     {"PSC ROM: I2C external message initialization failed",
      "Check hardware connections and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomBootModeSelFail),
     {"PSC ROM: Boot mode selection failed",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomUsbExtMsgFail),
     {"PSC ROM: USB external message initialization failed",
      "Check USB connection and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomErotGrantFail),
     {"PSC ROM: EROT grant failed",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomQspi0DevFail),
     {"PSC ROM: QSPI0 device initialization failed",
      "Check hardware and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomUsb2DevFail),
     {"PSC ROM: USB2 device initialization failed",
      "Check USB connection and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomOcprcDevFail),
     {"PSC ROM: OCP recovery device initialization failed",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomBootChainExhaust),
     {"PSC ROM: Boot chain exhausted - all boot options failed",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomBootImageLoadFail),
     {"PSC ROM: Boot image load failed",
      "Check image files are valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomDotSlotExhaust),
     {"PSC ROM: DOT slot exhausted",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomS2aHeaderCheckFail),
     {"PSC ROM: S2A header check failed",
      "Check S2A blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomS2aSanityFail),
     {"PSC ROM: S2A sanity check failed",
      "Check S2A blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomS2aIntegrityFail),
     {"PSC ROM: S2A integrity check failed",
      "Check S2A blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomS2aKeyRevoked),
     {"PSC ROM: S2A key has been revoked",
      "Use a package with valid signing keys."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomMutableDotHeaderCheckFail),
     {"PSC ROM: Mutable DOT header check failed",
      "Check DOT blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomMutableDotIntegrityFail),
     {"PSC ROM: Mutable DOT integrity check failed",
      "Check DOT blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomMutableDotSanityFail),
     {"PSC ROM: Mutable DOT sanity check failed",
      "Check DOT blob is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomVolatileDotParityFail),
     {"PSC ROM: Volatile DOT parity check failed",
      "Check DOT status and DOT blob is valid, then try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomVolatileDotHeaderCheckFail),
     {"PSC ROM: Volatile DOT header check failed",
      "Check DOT status and DOT blob is valid, then try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomVolatileDotSanityFail),
     {"PSC ROM: Volatile DOT sanity check failed",
      "Check DOT status and DOT blob is valid, then try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomCaliptraAckFail),
     {"PSC ROM: Caliptra acknowledgment failed",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomFmcCshSanityFail),
     {"PSC ROM: FMC CSH sanity check failed",
      "Check FMC image is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomFmcCshAuthz1Fail),
     {"PSC ROM: FMC CSH stage 1 authentication failed",
      "Check FMC image is properly signed and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomFmcCshAuthz2Fail),
     {"PSC ROM: FMC CSH stage 2 authentication failed",
      "Check FMC image is properly signed and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcCshBinListIntegrityFail),
     {"PSC ROM: FMC CSH binary list integrity check failed",
      "Check FMC image is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscRomFmcImageSanityFail),
     {"PSC ROM: FMC image sanity check failed",
      "Check FMC image is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageIntegrityFail),
     {"PSC ROM: FMC image integrity check failed",
      "Check FMC image is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageDecryptionFail),
     {"PSC ROM: FMC image decryption failed",
      "Check FMC image is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageOemRatchetFail),
     {"PSC ROM: FMC image OEM ratchet check failed - anti-rollback violation",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageMutDotSvnFuseRatchetFail),
     {"PSC ROM: FMC image mutable DOT SVN fuse ratchet check failed",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageMutDotSvnCshRatchetFail),
     {"PSC ROM: FMC image mutable DOT SVN CSH ratchet check failed",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageVolDotSvnFuseRatchetFail),
     {"PSC ROM: FMC image volatile DOT SVN fuse ratchet check failed",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageVolDotSvnCshRatchetFail),
     {"PSC ROM: FMC image volatile DOT SVN CSH ratchet check failed",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscRomFmcImageVolDotCshRatchetFail),
     {"PSC ROM: FMC image volatile DOT CSH ratchet check failed",
      "Use a newer firmware version that meets anti-rollback requirements."}},

    // PSC FMC Error Codes
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcFuseCrcFailed),
     {"PSC FMC: Fuse CRC check failed",
      "Hardware error detected. Contact NVIDIA support."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcLpiLsLinkFailed),
     {"PSC FMC: LPI LS link initialization failed",
      "Check hardware and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcBootChainLedgerInvalid),
     {"PSC FMC: Boot chain ledger is invalid",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcBootChainLedgerNotBootable),
     {"PSC FMC: Boot chain ledger indicates no bootable image",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcBootChainLedgerMismatch),
     {"PSC FMC: Boot chain ledger mismatch",
      "Try recovery again using the correct package."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcDebugTokenSanityFail),
     {"PSC FMC: Debug token sanity check failed",
      "Check debug token is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcDebugTokenAuthenticationFail),
     {"PSC FMC: Debug token authentication failed",
      "Check debug token is properly signed and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcSanityFailed),
     {"PSC FMC: Firmware sanity check failed",
      "Check firmware image is valid and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcStage1AuthenticationFailed),
     {"PSC FMC: Stage 1 firmware authentication failed",
      "Check firmware image is properly signed and try recovery again."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcStage2AuthenticationFailed),
     {"PSC FMC: Stage 2 firmware authentication failed",
      "Check firmware image is properly signed and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcSvnCheckFailed),
     {"PSC FMC: SVN (Security Version Number) check failed - anti-rollback violation",
      "Use a newer firmware version that meets anti-rollback requirements."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcHaltDisabledSocket),
     {"PSC FMC: Halted due to disabled socket",
      "Verify CPU socket is properly seated and enabled in BIOS/UEFI settings."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcMemFuseCrcFailed),
     {"PSC FMC: Memory fuse CRC check failed",
      "Hardware error detected. Contact NVIDIA support."}},
    {static_cast<ErrorCode>(
         USBRCMRecoveryErrorCode::PscFmcCaliptraMailboxFailed),
     {"PSC FMC: Caliptra mailbox communication failed",
      "Check device state and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcCsaFailed),
     {"PSC FMC: CSA (Chip Security Architecture) validation failed",
      "Check firmware image is valid and try recovery again."}},
    {static_cast<ErrorCode>(USBRCMRecoveryErrorCode::PscFmcDotFailed),
     {"PSC FMC: DOT validation failed",
      "Check DOT blob is valid and try recovery again."}},

    // Generic error codes
    {deviceNotResponding,
     {"Device is not responding",
      "Check USB connection and try recovery again."}},
    {deviceRecoveryFailed,
     {"Recovery failed due to unknown error",
      "Check device state and try recovery again using the correct package."}},
    {noDevicesFound, {"No Devices found to recover", ""}},
};

static const RecoveryErrorMapping recoveryMappingTbl = {
    {RecoveryProtocol::GlacierRecovery, glacierRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecovery, ocpRecoveryErrorMapping},
    {RecoveryProtocol::OCPRecoveryStatusError, ocpRecoveryStatusErrorMapping},
    {RecoveryProtocol::OCPDeviceStatusCode, ocpDeviceStatusErrorMapping},
    {RecoveryProtocol::OCPRecoveryProtocolError,
     ocpRecoveryProtocolErrorMapping},
    {RecoveryProtocol::MCURecovery, mcuRecoveryErrorMapping},
    {RecoveryProtocol::USBRCMRecovery, usbRcmRecoveryErrorMapping},
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
