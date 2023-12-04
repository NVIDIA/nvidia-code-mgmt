#pragma once
#include "i2c_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace glacier_recovery_tool
{
namespace glacier_recovery_commands
{

constexpr bool Tx = true;
constexpr bool Rx = false;
static constexpr uint8_t delay1sec = 1;
static constexpr uint8_t maxRetries = 5;

constexpr size_t defaultChunkSize = 128;
constexpr size_t revAKHBSize = 1104;
constexpr size_t revBKHBSize = 576;
constexpr size_t headerLength = 896;
constexpr size_t randomByteCount = 32;
constexpr uint8_t initResponseByte0 = 0xAA;
constexpr uint8_t initResponseByte1 = 0x5A;
constexpr uint8_t initResponseByte2 = 0xA5;
constexpr uint8_t crcFailField = 0x1;
constexpr uint8_t illegalLengthField = 0x2;
constexpr uint8_t illegalOffsetField = 0x4;
constexpr uint8_t invalidCmdSignatureField = 0x8;
constexpr size_t offsetBytes = 3;
constexpr size_t payloadLengthBytes = 1;

/**
 * @enum RecoveryCommands
 * @brief Enumerates commands for Glacier recovery.
 */
enum class RecoveryCommand : uint8_t
{
    Initialization = 0x55,
    HeaderWrite = 0x65,
    KeyHashBlobWrite = 0x66,
    FWImageWrite = 0x67,
    SRAMExe = 0x69,
    GetFWInfo = 0x70,
    GetResponse = 0x71,
    ShowHiddenERoTs = 0x60
};

/**
 * @enum ImageType
 * @brief Enumerates different types of supported firmware revisions.
 */
enum class Revision : uint8_t
{
    RevA = 0x0,
    RevB = 0x1,
    RevUnknown = 0xFF
};

/**
 * @enum RecoveryCommands
 * @brief Enumerates different possible results or status for the operations
 * used in Glacier recovery processes.
 */
enum class RecoveryResult : uint8_t
{
    Ok = 0x0,
    IllegalPayloadLength = 0x1,
    CRCFailure = 0x2,
    ResponseCRCCompFailure = 0x3,
    IllegalHeaderOffset = 0x4,
    IllegalKeyHashBlobOffset = 0x5,
    IllegalFWImageWriteAddress = 0x6,
    InvalidCommandSignature = 0x7,
    DeviceNotInRecovery = 0x8,
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
    FailedToReadFWImage = 0x14
};

/**
 * @enum ResponseLength
 * @brief Enumerates the expected response lengths for various recovery
 * commands.
 */
enum class ResponseLength : std::size_t
{
    WriteResponse = 7,
    InitResponse = 8,
    FwInfoRevAResponse = 18,
    FwInfoRevBResponse = 26
};

/** @brief Compute Crc32(same as the one used by IEEE802.3)
 *
 *  @param[in] data - Pointer to the target data
 *  @param[in] size - Size of the data
 *  @return The checksum
 */
uint32_t crc32(const void* data, size_t size);
class GlacierRecoveryCommands
{
  private:
    int busAddress;
    int slaveAddress;
    bool verbose;
    int i2cFile;
    Revision revision = Revision::RevB;

    /**
     * @brief Opens the I2C device for communication.
     */
    void openI2CDevice();
    /**
     * @brief Generates a command byte sequence with an appended CRC32 checksum.
     *
     * @param recoveryCommand The specific recovery command for which the byte
     * sequence needs to be generated.
     * @param payload payload data which is to be written.
     * @return A std::vector<uint8_t> containing the command bytes followed by
     * the payload and ending with a CRC32 checksum.
     */
    std::vector<uint8_t>
        getCommandBytesWithCRC32(RecoveryCommand recoveryCommand,
                                 const std::vector<uint8_t>& payload);
    /**
     * @brief Validates the response received from GetStatus command.
     *
     * @param command The recovery command for which the response is being
     * validated.
     * @param responseBytes The
     *                      response bytes from the Get Status command.
     * @param responseLen The expected length of the response for the given
     * recovery command.
     * @return A RecoveryResult indicating the outcome of the validation.
     */
    RecoveryResult
        ValidateGetResponseCmd(RecoveryCommand command,
                               const std::vector<uint8_t>& responseBytes,
                               ResponseLength responseLen);
    /**
     * @brief Retrieves the status of the last sent command from the
     * device.
     *
     * @param responseLen The expected length of the response to read from the
     * device.
     * @return A std::vector<uint8_t> containing the bytes read from the device
     * as the response.
     */
    std::vector<uint8_t> GetResponse(ResponseLength responseLen);
    /**
     * @brief Perform specific validation on the GetResponse outputs for write
     * Image Commands.
     *
     * @param cmd The specific command related to image writing for which the
     * response is validated.
     * @param responseBytes The
     *                      response bytes from the Get Status command.
     * @return An enum of type RecoveryResult indicating the outcome of the
     * validation.
     */
    RecoveryResult
        ValidateWriteImageResponse(RecoveryCommand cmd,
                                   const std::vector<uint8_t>& responseBytes);
    /**
     * @brief Executes the Get Status recovery command and validates the
     * response.
     *
     * @param cmd The recovery command to execute.
     * @param responseLen The expected length of the response.
     * @return A RecoveryResult indicating the outcome of the operation.
     */
    RecoveryResult
        executeGetResponseCmdAndValidateResponse(RecoveryCommand cmd,
                                                 ResponseLength responseLen);
    /**
     * @brief Command used to trigger BootROM to authenticate, optionally
     * decrypt and execute the crisis recovery image.
     *
     * @return A boolean indicating whether the SRAM_EXE command was
     * successfully initiated.
     */
    bool performSRAMExe();
    /**
     * @brief Writes the specific image to the device as part of the recovery
     * process.
     *
     * @param cmd The command used to initiate the write operation.
     * @param size The size of the image data to write.
     * @param data A vector containing the image data.
     * @return An enum of type RecoveryResult.
     */
    RecoveryResult writeImage(RecoveryCommand cmd, size_t size,
                              const std::vector<uint8_t>& data);
    /**
     * @brief Executes the command to write a portion of an image to the device
     * and validates the response.
     *
     * @param command The recovery command associated with writing image data.
     * @param payload The data chunk to be written.
     * @param offset The offset at which to write the payload within the image.
     * @return An enum of type RecoveryResult.
     */
    RecoveryResult executeWriteImageCmd(RecoveryCommand command,
                                        const std::vector<uint8_t>& payload,
                                        size_t offset);
    /**
     * @brief Generates the payload for a write command.
     *
     * @param payload The original payload data to be included in the write
     * command.
     * @param offset The offset at which the payload should be applied within
     * the target memory.
     * @return A vector of bytes representing the complete payload for the write
     * command.
     */
    std::vector<uint8_t>
        getWriteCommandPayload(const std::vector<uint8_t>& payload,
                               size_t offset);
    /** @brief Print the buffer
     *
     *  @param isTx - True if the buffer is the command data written to the
     *device, false if the buffer is the data read from the device.
     *  @param buffer - Buffer to print
     */
    void printBuffer(bool isTx, const std::vector<uint8_t>& buffer);
    /* @brief Converts a size value into a vector of bytes

    * @param value The size or offset value to be converted.
    * @return A vector of three bytes representing the input value in
    little-endian format.
    */
    std::vector<uint8_t> toOffsetBytes(size_t value);

  public:
    /**
     * @brief Constructs a GlacierRecoveryCommands object with specified I2C
     * settings.
     *
     * @param busAddr The bus address for I2C communication.
     * @param slaveAddr The slave address for I2C communication.
     * @param verbose Set to true to enable verbose logging.
     */
    GlacierRecoveryCommands(int busAddr, int slaveAddr, bool verbose);

    GlacierRecoveryCommands() = delete;
    GlacierRecoveryCommands(const GlacierRecoveryCommands&) = delete;
    GlacierRecoveryCommands(GlacierRecoveryCommands&&) = delete;
    GlacierRecoveryCommands& operator=(const GlacierRecoveryCommands&) = delete;
    GlacierRecoveryCommands& operator=(GlacierRecoveryCommands&&) = delete;
    ~GlacierRecoveryCommands();
    /**
     * @brief Get the firmware information of the device.
     *
     * @return A tuple containing an enum of type RecoveryResult and firmware
     * information as a byte vector.
     */
    std::tuple<RecoveryResult, std::vector<unsigned char>>
        getFirmwareInfoCommand();
    /**
     * @brief Perform initialization command to establish i2c communication.
     *
     * @return An enum of type RecoveryResult.
     */
    RecoveryResult performInitialization();
    /**
     * @brief Initiates the recovery process using the specified image paths.
     *
     * @param imagePaths A list of image paths to use for recovery.
     * @return An enum of type RecoveryResult.
     */
    RecoveryResult performGlacierRecovery(const std::string& imgFilePath);
    /**
     * @brief Log a message if verbose mode is enabled.
     *
     * @param message The message to log.
     */
    void logVerbose(const std::string& message) const;
    /**
     * @brief The aggregate command to show hidden ERoTs.
     *
     * @return A boolean indicating whether the unlockI2CDevice command was
     * successfully initiated.
     */
    bool unlockI2CDevice();
    /**
     * @brief Converts a RecoveryResult enum value into a human-readable string.
     *
     * @param result The RecoveryResult value to be converted.
     * @return A string representation of the RecoveryResult.
     */
    std::string recoveryResultToStr(RecoveryResult result);
};

} // namespace glacier_recovery_commands
} // namespace glacier_recovery_tool
