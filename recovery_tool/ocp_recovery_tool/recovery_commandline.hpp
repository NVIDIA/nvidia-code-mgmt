#pragma once

#include "message_registry.hpp"

#include <nlohmann/json.hpp>
#include <sdbusplus/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <tuple>

namespace recovery_tool
{
namespace recovery_commands
{
class OCPRecoveryCommands;
enum class OperationalStatus : uint8_t;
} // namespace recovery_commands
} // namespace recovery_tool

using OperationalStatus = recovery_tool::recovery_commands::OperationalStatus;

namespace ocp_recovery_commandline
{

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

static constexpr uint8_t delay1sec = 1;

/**
 * @enum RecoveryReturnCode
 * @brief Represents the status of ocp recovery
 *
 * @var SUCCESS indicates a successful recovery
 * @var SKIPPED indicates that recovery operation was skipped for a GPU owing
 *              to the device not being in recovery before trying to perform
 *              recovery
 * @var FAILURE indicates a failed recovery
 */
enum class RecoveryReturnCode : int
{
    SUCCESS = 0,
    SKIPPED = 1,
    FAILURE = 2
};

/**
 * @enum DeviceStatus
 * @brief Represents the device status of GetDeviceStatus command
 */
enum class DeviceStatusCode : int
{
    CommandFailure = -1,
    StatusPending =
        0X0, // Device is booting or has not yet populated the reason code
    DeviceHealthy = 0x1,
    DeviceError = 0x2,
    RecoveryMode =
        0x3, // Device has entered recovery based on forced recovery, or error
             // in the previous boot. Device is ready to accept recovery image
    RecoveryPending = 0x4, // Waiting for device/platform reset
    RecoveryImgRunning = 0x5,
    // RESERVED                 = 0x6 to 0xD
    BootFailure = 0xE, // Device current boot is halted. The reason is defined
                       // in the recovery reason code.
    FatalError = 0xF
};

/**
 * @enum ProtocolError
 * @brief Represents protocol error of GetDeviceStatus command.
 */
enum class ProtocolError : int
{
    NoProtocolError =
        0x0, // Device is booting or has not yet populated the reason code
    UnsupportedWriteCommand =
        0x1, // Command is not supported or is a write to a RO command
    UnsupportedParameter = 0x2,
    LengthWriteError = 0x3, // Length of write command is incorrect
    CrcError = 0x4,         // If supported
    DeviceNotResponding = 0x5,
    // Reserved values range from 0x5 to 0xFE
    GeneralProtocolError = 0xFF // Catch all unclassified errors
};

/**
 * @enum RecoveryReasonCode
 * @brief Represents recovery reason codes for GetDeviceStatus command.
 */
enum class RecoveryReasonCode : int
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

/**
 * @enum RecoveryStatus
 * @brief Represents the recovery status of the GetRecoveryStatus command.
 */
enum class RecoveryStatus : int
{
    CommandFailure = -1,
    NotInRecoveryMode = 0x0,
    AwaitingRecoveryImg = 0x1,
    BootingRecoveryImg = 0x2,
    RecoverySuccess = 0x3,
    RecoveryFailed = 0xC,
    RecoveryImgAuthFailed = 0xD,
    ErrorEnteringRecoveryMode = 0xE,
    InvalidCms = 0xF
};

/**
 * @enum DeviceStatus
 * @brief Represents the device status of GetDeviceStatus command
 */
struct DeviceStatus
{
    DeviceStatusCode statusCode;
    RecoveryReasonCode recoveryReason;
    ProtocolError protocolError;
};

/**
 * @class OCPRecoveryCommandLine
 * @brief Utility for managing and performing recovery operations on devices
 * that support OCP recovery protocol
 *
 * This class provides functionalities for querying device status,
 * getting recovery status, and performing recovery operations.
 */
class OCPRecoveryCommandLine
{

  public:
    /**
     * @brief Constructor with parameters.
     * @param device Name of the device
     * @param busAddr The bus address.
     * @param slaveAddr The slave address.
     * @param verb Verbose logging flag.
     * @param emul Emulation mode flag.
     */
    OCPRecoveryCommandLine(const std::string_view device, int busAddr,
                           int slaveAddr, bool verb, bool emul = false);
    OCPRecoveryCommandLine() = delete;
    OCPRecoveryCommandLine(const OCPRecoveryCommandLine&) = delete;
    OCPRecoveryCommandLine(OCPRecoveryCommandLine&&) = delete;
    OCPRecoveryCommandLine& operator=(const OCPRecoveryCommandLine&) = delete;
    OCPRecoveryCommandLine& operator=(OCPRecoveryCommandLine&&) = delete;
    ~OCPRecoveryCommandLine() = default;
    /**
     * @brief Retrieves the device's status in JSON format.
     * @return A JSON object representing the device's status.
     */
    DeviceStatus getDeviceStatus() const noexcept;

    /**
     * @brief Retrieves the recovery status and recovery image index.
     * @return A pair of RecoveryStatus and Recovery Image Index (uint8_t).
     */
    std::pair<RecoveryStatus, uint8_t> getRecoveryStatus() const noexcept;

    /**
     * @brief Initiates the recovery process using the specified image paths.
     * @param imagePaths A list of image paths to use for recovery.
     * @return true on failure and false on successful recovery
     */
    RecoveryReturnCode performRecovery(
        const std::vector<std::string>& imagePaths) const noexcept;

  private:
    /**
     * @brief Check whether the GPU is operational state
     * @return A JSON object representing the recovery status.
     */
    std::tuple<OperationalStatus, DeviceStatusCode, ProtocolError,
               RecoveryStatus>
        getOperationalStatus() const noexcept;

    /**
     * @brief Converts DeviceStatus enumeration to its string representation.
     * @param status The DeviceStatus value.
     * @return The string representation of DeviceStatus.
     */
    std::string deviceStatusToStr(DeviceStatusCode status) const noexcept;

    /**
     * @brief Converts ProtocolError enumeration to its string representation.
     * @param error The ProtocolError value.
     * @return The string representation of ProtocolError.
     */
    std::string protocolErrorToStr(ProtocolError error) const noexcept;

    /**
     * @brief Converts RecoveryReasonCode enumeration to its string
     * representation.
     * @param code The RecoveryReasonCode value.
     * @return The string representation of RecoveryReasonCode.
     */
    std::string recoveryReasonCodeToStr(RecoveryReasonCode code) const noexcept;

  private:
    bool verbose;
    std::string device;
    std::unique_ptr<recovery_tool::recovery_commands::OCPRecoveryCommands>
        recoveryCommands;
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    MessageRegistry registry;
};

} // namespace ocp_recovery_commandline
