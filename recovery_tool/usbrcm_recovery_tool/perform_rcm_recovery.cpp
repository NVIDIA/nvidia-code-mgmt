/**
 * @file perform_rcm_recovery.cpp
 * @brief PerformUSBRecovery implementation - USB RCM image transfer via libusb
 */

#include "perform_rcm_recovery.hpp"

#include "ecid_parser.hpp"
#include "message_registry.hpp"
#include "progress_code_parser.hpp"
#include "progress_code_queue.hpp"
#include "usb_device_manager.hpp"
#include "usb_io.hpp"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace
{

/// RCM endpoint and transfer constants
constexpr uint8_t RCM_BULK_OUT_ENDPOINT =
    0x08; ///< Bulk OUT endpoint for images
constexpr int BULK_TRANSFER_TIMEOUT_MS = 30000; ///< 30 second timeout
constexpr size_t BULK_CHUNK_SIZE = 512; ///< 512B chunks for optimal throughput

/// Progress monitoring timing constants
constexpr int PROGRESS_CHECK_INTERVAL_MS = 500; ///< Check progress every 500ms
constexpr int POST_IMAGE_MONITOR_ITERATIONS =
    8; ///< Monitor for 4 seconds after image (8 × 500ms)
constexpr int INTER_IMAGE_DELAY_MS = 250; ///< Pause between images

/// Progress check result status
enum class ProgressStatus
{
    Continue,  ///< No error, no completion
    Completed, ///< Recovery completed
    Error      ///< Error detected
};

/**
 * @brief Convert PSC ROM error operation code to USBRCMRecoveryErrorCode
 * @param operation PSC ROM error operation code (bits 5:0)
 * @return Corresponding USBRCMRecoveryErrorCode
 */
[[nodiscard]] USBRCMRecoveryErrorCode
    pscRomErrorToRecoveryErrorCode(uint8_t operation) noexcept
{
    switch (operation)
    {
        case 0x01:
            return USBRCMRecoveryErrorCode::PscRomI2cExtMsgFail;
        case 0x02:
            return USBRCMRecoveryErrorCode::PscRomBootModeSelFail;
        case 0x03:
            return USBRCMRecoveryErrorCode::PscRomUsbExtMsgFail;
        case 0x04:
            return USBRCMRecoveryErrorCode::PscRomErotGrantFail;
        case 0x05:
            return USBRCMRecoveryErrorCode::PscRomQspi0DevFail;
        case 0x06:
            return USBRCMRecoveryErrorCode::PscRomUsb2DevFail;
        case 0x07:
            return USBRCMRecoveryErrorCode::PscRomOcprcDevFail;
        case 0x08:
            return USBRCMRecoveryErrorCode::PscRomBootChainExhaust;
        case 0x09:
            return USBRCMRecoveryErrorCode::PscRomBootImageLoadFail;
        case 0x0A:
            return USBRCMRecoveryErrorCode::PscRomDotSlotExhaust;
        case 0x0B:
            return USBRCMRecoveryErrorCode::PscRomS2aHeaderCheckFail;
        case 0x0C:
            return USBRCMRecoveryErrorCode::PscRomS2aSanityFail;
        case 0x0D:
            return USBRCMRecoveryErrorCode::PscRomS2aIntegrityFail;
        case 0x0E:
            return USBRCMRecoveryErrorCode::PscRomS2aKeyRevoked;
        case 0x0F:
            return USBRCMRecoveryErrorCode::PscRomMutableDotHeaderCheckFail;
        case 0x10:
            return USBRCMRecoveryErrorCode::PscRomMutableDotIntegrityFail;
        case 0x11:
            return USBRCMRecoveryErrorCode::PscRomMutableDotSanityFail;
        case 0x12:
            return USBRCMRecoveryErrorCode::PscRomVolatileDotParityFail;
        case 0x13:
            return USBRCMRecoveryErrorCode::PscRomVolatileDotHeaderCheckFail;
        case 0x14:
            return USBRCMRecoveryErrorCode::PscRomVolatileDotSanityFail;
        case 0x15:
            return USBRCMRecoveryErrorCode::PscRomCaliptraAckFail;
        case 0x16:
            return USBRCMRecoveryErrorCode::PscRomFmcCshSanityFail;
        case 0x17:
            return USBRCMRecoveryErrorCode::PscRomFmcCshAuthz1Fail;
        case 0x18:
            return USBRCMRecoveryErrorCode::PscRomFmcCshAuthz2Fail;
        case 0x19:
            return USBRCMRecoveryErrorCode::PscRomFmcCshBinListIntegrityFail;
        case 0x1A:
            return USBRCMRecoveryErrorCode::PscRomFmcImageSanityFail;
        case 0x1B:
            return USBRCMRecoveryErrorCode::PscRomFmcImageIntegrityFail;
        case 0x1C:
            return USBRCMRecoveryErrorCode::PscRomFmcImageDecryptionFail;
        case 0x1D:
            return USBRCMRecoveryErrorCode::PscRomFmcImageOemRatchetFail;
        case 0x1E:
            return USBRCMRecoveryErrorCode::
                PscRomFmcImageMutDotSvnFuseRatchetFail;
        case 0x1F:
            return USBRCMRecoveryErrorCode::
                PscRomFmcImageMutDotSvnCshRatchetFail;
        case 0x20:
            return USBRCMRecoveryErrorCode::
                PscRomFmcImageVolDotSvnFuseRatchetFail;
        case 0x21:
            return USBRCMRecoveryErrorCode::
                PscRomFmcImageVolDotSvnCshRatchetFail;
        case 0x22:
            return USBRCMRecoveryErrorCode::PscRomFmcImageVolDotCshRatchetFail;
        default:
            return USBRCMRecoveryErrorCode::ImageTransferFailed;
    }
}

/**
 * @brief Convert PSC FMC error operation code to USBRCMRecoveryErrorCode
 * @param operation PSC FMC error operation code (bits 5:0)
 * @return Corresponding USBRCMRecoveryErrorCode
 */
[[nodiscard]] USBRCMRecoveryErrorCode
    pscFmcErrorToRecoveryErrorCode(uint8_t operation) noexcept
{
    switch (operation)
    {
        case 0x01:
            return USBRCMRecoveryErrorCode::PscFmcFuseCrcFailed;
        case 0x02:
            return USBRCMRecoveryErrorCode::PscFmcLpiLsLinkFailed;
        case 0x03:
            return USBRCMRecoveryErrorCode::PscFmcBootChainLedgerInvalid;
        case 0x04:
            return USBRCMRecoveryErrorCode::PscFmcBootChainLedgerNotBootable;
        case 0x05:
            return USBRCMRecoveryErrorCode::PscFmcBootChainLedgerMismatch;
        case 0x06:
            return USBRCMRecoveryErrorCode::PscFmcDebugTokenSanityFail;
        case 0x07:
            return USBRCMRecoveryErrorCode::PscFmcDebugTokenAuthenticationFail;
        case 0x08:
            return USBRCMRecoveryErrorCode::PscFmcSanityFailed;
        case 0x09:
            return USBRCMRecoveryErrorCode::PscFmcStage1AuthenticationFailed;
        case 0x0A:
            return USBRCMRecoveryErrorCode::PscFmcStage2AuthenticationFailed;
        case 0x0B:
            return USBRCMRecoveryErrorCode::PscFmcSvnCheckFailed;
        case 0x0C:
            return USBRCMRecoveryErrorCode::PscFmcHaltDisabledSocket;
        case 0x0D:
            return USBRCMRecoveryErrorCode::PscFmcMemFuseCrcFailed;
        case 0x0E:
            return USBRCMRecoveryErrorCode::PscFmcCaliptraMailboxFailed;
        case 0x0F:
            return USBRCMRecoveryErrorCode::PscFmcCsaFailed;
        case 0x10:
            return USBRCMRecoveryErrorCode::PscFmcDotFailed;
        default:
            return USBRCMRecoveryErrorCode::ImageTransferFailed;
    }
}

/**
 * @brief Convert progress code error to USBRCMRecoveryErrorCode
 * @param progressCode 32-bit progress code
 * @return Corresponding USBRCMRecoveryErrorCode
 */
[[nodiscard]] USBRCMRecoveryErrorCode
    progressCodeToRecoveryErrorCode(uint32_t progressCode) noexcept
{
    const auto info = ProgressCodeParser::getProgressCodeInfo(progressCode);

    if (info.type != ProgressCodeParser::CodeType::Error)
    {
        return USBRCMRecoveryErrorCode::ImageTransferFailed;
    }

    switch (info.subClass)
    {
        case ProgressCodeParser::SubClass::PSC_ROM:
            return pscRomErrorToRecoveryErrorCode(info.operation);
        case ProgressCodeParser::SubClass::PSC_FMC:
            return pscFmcErrorToRecoveryErrorCode(info.operation);
        default:
            return USBRCMRecoveryErrorCode::ImageTransferFailed;
    }
}

/**
 * @brief Check if data is the expected "empty" DOT blob (1024 bytes of zeros).
 *        Used in DOT recovery override flow where PSC reports
 *        PSC_ROM_EC_MUTABLE_DOT_HEADER_CHECK_FAIL for this payload; we treat
 *        it as expected and continue recovery instead of aborting.
 * @param data Image data that was sent
 * @return true if data is exactly 1024 bytes and all zeros
 */
[[nodiscard]] bool isEmptyDotBlob(const std::vector<uint8_t>& data) noexcept
{
    constexpr size_t emptyDotBlobSize = 1024; // PSC expects exactly 1024B
    return data.size() == emptyDotBlobSize &&
           std::all_of(data.begin(), data.end(),
                       [](uint8_t b) { return b == 0; });
}

[[nodiscard]] bool readFileToBuffer(const std::string& filePath,
                                    std::vector<uint8_t>& buffer, bool verbose)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file)
    {
        if (verbose)
        {
            std::cerr << std::format("Failed to open file: {}\n", filePath);
        }
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        if (verbose)
        {
            std::cerr << std::format("Invalid file size: {}\n", filePath);
        }
        return false;
    }

    file.seekg(0, std::ios::beg);
    buffer.resize(static_cast<size_t>(fileSize));

    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize))
    {
        if (verbose)
        {
            std::cerr << std::format("Failed to read file: {}\n", filePath);
        }
        return false;
    }

    return true;
}

/**
 * @brief Send data via USB bulk transfer (synchronous)
 *
 * Sends data in 512-byte chunks. Each transfer has a 30-second timeout
 * to handle slow or stalled USB connections.
 *
 * @param handle USB device handle
 * @param data Pointer to data buffer to send
 * @param size Size of data to send in bytes
 * @param verbose Enable progress output to stdout and errors to stderr
 * @param outLibusbError Optional: set to libusb error code on transfer failure
 * (e.g. LIBUSB_ERROR_TIMEOUT); unchanged on success
 *
 * @return true if all data sent successfully, false on transfer failure
 */
[[nodiscard]] bool sendBulkData(libusb_device_handle* handle,
                                const uint8_t* data, size_t size, bool verbose,
                                std::optional<int>* outLibusbError = nullptr)
{
    if (outLibusbError)
    {
        *outLibusbError = std::nullopt;
    }
    size_t totalSent = 0;

    while (totalSent < size)
    {
        const size_t chunkSize = std::min(BULK_CHUNK_SIZE, size - totalSent);
        int actualLength = 0;

        const int ret = libusb_bulk_transfer(
            handle, RCM_BULK_OUT_ENDPOINT,
            const_cast<uint8_t*>(data + totalSent), static_cast<int>(chunkSize),
            &actualLength, BULK_TRANSFER_TIMEOUT_MS);

        if (ret != LIBUSB_SUCCESS)
        {
            if (outLibusbError)
            {
                *outLibusbError = ret;
            }
            if (verbose)
            {
                std::cerr << std::format(
                    "Bulk transfer failed: {} ({})\n", libusb_error_name(ret),
                    libusb_strerror(static_cast<libusb_error>(ret)));
            }
            return false;
        }

        if (actualLength <= 0)
        {
            if (verbose)
            {
                std::cerr
                    << "Bulk transfer succeeded but transferred 0 bytes\n";
            }
            return false;
        }

        totalSent += actualLength;

        if (verbose)
        {
            std::cout << std::format("  Transfer completed: {} bytes\n",
                                     actualLength);
        }

        if (verbose && size > BULK_CHUNK_SIZE)
        {
            std::cout << std::format("  Progress: {} / {} bytes ({:.1f}%)\n",
                                     totalSent, size,
                                     (totalSent * 100.0) / size);
        }
    }

    return true;
}

/**
 * @brief Check for new progress codes and display them
 *
 * Retrieves new progress codes since last check and evaluates each one:
 * - Checks for recovery completion
 * - Checks for error codes
 * - Displays codes in verbose mode
 * - Updates lastProcessedTimestamp for successfully processed codes
 *
 * @param handle USB device handle for reading progress codes
 * @param lastProcessedTimestamp Reference to timestamp of last processed code;
 * updated as new codes are processed
 * @param errorDetail Reference to error message string; populated if error code
 * detected
 * @param errorCode Reference to error code; populated if error detected
 * @param verbose Enable diagnostic output to stdout/stderr
 *
 * @return ProgressStatus::Completed if recovery completion code detected
 * @return ProgressStatus::Error if error code detected
 * @return ProgressStatus::Continue if no completion or error found
 */
[[nodiscard]] ProgressStatus checkForNewProgressCodes(
    libusb_device_handle* handle, uint32_t& lastProcessedTimestamp,
    std::string& errorDetail, USBRCMRecoveryErrorCode& errorCode, bool verbose)
{
    std::vector<progress_queue::CPUProgressLogEntry> entries;

    if (!progress_queue::readCpuPcqEntriesAfter(handle, lastProcessedTimestamp,
                                                entries, false))
    {
        return ProgressStatus::Continue;
    }

    if (entries.empty())
    {
        return ProgressStatus::Continue;
    }

    for (const auto& entry : entries)
    {
        if (entry.progressCode == 0)
            continue;

        const auto info =
            ProgressCodeParser::getProgressCodeInfo(entry.progressCode);

        if (verbose)
        {
            if (info.type == ProgressCodeParser::CodeType::Error)
            {
                std::cerr << std::format(
                    "ERROR [CPU{}]: {}\n",
                    ProgressCodeParser::packageClassToCpuId(info.cpuId),
                    info.name);
            }
            else
            {
                std::cout << std::format(
                    "[CPU{}] {}\n",
                    ProgressCodeParser::packageClassToCpuId(info.cpuId),
                    info.name);
            }
        }

        if (ProgressCodeParser::isRecoveryComplete(entry.progressCode))
        {
            if (verbose)
            {
                std::cout << "Recovery completion detected!\n";
            }
            return ProgressStatus::Completed;
        }

        if (info.type == ProgressCodeParser::CodeType::Error)
        {
            errorDetail = std::format("Recovery failed: {}", info.name);
            errorCode = progressCodeToRecoveryErrorCode(entry.progressCode);
            return ProgressStatus::Error;
        }

        lastProcessedTimestamp = entry.timestamp;
    }

    return ProgressStatus::Continue;
}

/**
 * @brief Monitor progress codes after sending an image
 *
 * Polls for new progress codes every 500ms. Default is 4 seconds (8
 * iterations). Returns immediately if completion code or error is detected
 * during monitoring.
 *
 * @param handle USB device handle for reading progress codes
 * @param lastProcessedTimestamp Reference to timestamp of last processed code;
 * updated as new codes are processed
 * @param errorDetail Reference to error message string; populated if error code
 * detected
 * @param errorCode Reference to error code; populated if error detected
 * @param verbose Enable diagnostic output to stdout/stderr
 * @param iterations Number of 500ms poll iterations (default 4s)
 *
 * @return ProgressStatus::Completed if recovery completion detected
 * @return ProgressStatus::Error if error code detected
 * @return ProgressStatus::Continue if monitoring period elapses without
 * completion/error
 */
[[nodiscard]] ProgressStatus monitorProgressAfterImageTransfer(
    libusb_device_handle* handle, uint32_t& lastProcessedTimestamp,
    std::string& errorDetail, USBRCMRecoveryErrorCode& errorCode, bool verbose,
    int iterations = POST_IMAGE_MONITOR_ITERATIONS)
{
    using namespace std::chrono;

    for (int i = 0; i < iterations; ++i)
    {
        std::this_thread::sleep_for(milliseconds(PROGRESS_CHECK_INTERVAL_MS));

        ProgressStatus status = checkForNewProgressCodes(
            handle, lastProcessedTimestamp, errorDetail, errorCode, verbose);
        if (status != ProgressStatus::Continue)
        {
            return status;
        }
    }

    return ProgressStatus::Continue;
}

} // anonymous namespace

bool performUsbRecovery(const std::string& portPath,
                        const std::vector<std::string>& imagePaths,
                        const std::string& blobPath, nlohmann::json& jsonOutput,
                        bool verbose)
{
    jsonOutput = nlohmann::json::object();

    if (portPath.empty())
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = "Port path cannot be empty";
        jsonOutput["ErrorCode"] =
            static_cast<uint8_t>(USBRCMRecoveryErrorCode::DeviceNotFound);
        return false;
    }

    if (imagePaths.empty())
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = "At least one recovery image must be provided";
        jsonOutput["ErrorCode"] =
            static_cast<uint8_t>(USBRCMRecoveryErrorCode::InvalidImageOrder);
        return false;
    }

    try
    {
        usb::UsbContext ctx;
        if (!ctx.isValid())
        {
            jsonOutput["Status"] = "Failed";
            jsonOutput["Error"] = "Failed to initialize USB subsystem";
            jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                USBRCMRecoveryErrorCode::InitializationFailed);
            return false;
        }

        auto device = usb::findDeviceByPortPath(ctx, portPath, verbose);
        if (!device)
        {
            jsonOutput["Status"] = "Failed";
            jsonOutput["Error"] =
                std::format("Device not found at port path: {}", portPath);
            jsonOutput["ErrorCode"] =
                static_cast<uint8_t>(USBRCMRecoveryErrorCode::DeviceNotFound);
            return false;
        }

        usb::UsbDeviceHandle deviceHandle(device->get(), ctx,
                                          usb::INTERFACE_RECOVERY);
        if (!deviceHandle)
        {
            jsonOutput["Status"] = "Failed";
            jsonOutput["Error"] = std::format(
                "Failed to open USB device and claim RCM interface {}",
                usb::INTERFACE_RECOVERY);
            jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                USBRCMRecoveryErrorCode::USBCommunicationError);
            return false;
        }

        libusb_device_handle* rawHandle = deviceHandle.get();

        if (verbose)
        {
            std::cout << std::format(
                "Opened device at port {} and claimed RCM interface {}\n",
                portPath, usb::INTERFACE_RECOVERY);
        }

        std::vector<std::string> finalImagePaths;

        if (auto ecid = usb_io::readDeviceEcid(rawHandle, verbose))
        {
            const auto blobReq = EcidParser::getBlobRequirement(ecid->data());

            if (blobReq == EcidParser::BlobRequirement::S2ABlob)
            {
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] = "S2A blob required - not supported";
                jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                    USBRCMRecoveryErrorCode::S2ABlobNotSupported);
                return false;
            }

            if (blobReq == EcidParser::BlobRequirement::DOTBlob)
            {
                if (blobPath.empty())
                {
                    jsonOutput["Status"] = "Failed";
                    jsonOutput["Error"] = "DOT blob required but not provided";
                    jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                        USBRCMRecoveryErrorCode::DOTBlobRequired);
                    return false;
                }

                if (verbose)
                {
                    std::cout << "DOT blob required - will send blob first\n";
                }

                finalImagePaths.push_back(blobPath);
            }
            else
            {
                if (verbose)
                {
                    std::cout << "No blob required - standard recovery\n";
                }
            }
        }
        else
        {
            if (verbose)
            {
                std::cerr
                    << "Warning: Could not read ECID, proceeding without blob validation\n";
            }
        }

        finalImagePaths.insert(finalImagePaths.end(), imagePaths.begin(),
                               imagePaths.end());

        uint32_t lastProcessedTimestamp = 0;
        std::string errorDetail;
        USBRCMRecoveryErrorCode errorCode =
            USBRCMRecoveryErrorCode::ImageTransferFailed;

        // Track if we accepted the empty-DOT error; only set
        // EmptyDotBlobAccepted in output when recovery ultimately fails (no
        // completion code).
        bool emptyDotBlobAccepted = false;

        auto reportEmptyDotSoftSuccess = [&jsonOutput]() {
            jsonOutput["Status"] = "Successful";
            jsonOutput["EmptyDotBlobAccepted"] = true;
            return true;
        };

        for (size_t i = 0; i < finalImagePaths.size(); ++i)
        {
            const auto& imagePath = finalImagePaths[i];

            if (verbose)
            {
                std::cout << std::format("Sending image {} of {}: {}\n", i + 1,
                                         finalImagePaths.size(), imagePath);
            }

            std::vector<uint8_t> imageData;
            if (!readFileToBuffer(imagePath, imageData, verbose))
            {
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] =
                    std::format("Failed to read image file: {}", imagePath);
                jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                    USBRCMRecoveryErrorCode::FileOpenFailure);
                return false;
            }

            if (verbose)
            {
                std::cout << std::format("  Size: {} bytes\n",
                                         imageData.size());
            }

            // First image after the DOT blob (index 1 when blob is at 0). Used
            // only for soft-success on USB timeout.
            const bool firstImageAfterDOTBlob = (i == 1 && !blobPath.empty());
            std::optional<int> libusbErr;
            if (!sendBulkData(rawHandle, imageData.data(), imageData.size(),
                              verbose, &libusbErr))
            {
                // Soft success only when: we already accepted empty-DOT error,
                // this transfer is the first image after DOT blob, and failure
                // is USB timeout.
                if (emptyDotBlobAccepted && firstImageAfterDOTBlob &&
                    libusbErr.has_value() && *libusbErr == LIBUSB_ERROR_TIMEOUT)
                {
                    if (verbose)
                    {
                        std::cout << "Empty DOT blob flow: First image after "
                                     "empty DOT blob timed out, reporting task "
                                     "success with EnterDOTRecovery.\n";
                    }
                    return reportEmptyDotSoftSuccess();
                }
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] =
                    std::format("Failed to send image: {}", imagePath);
                jsonOutput["ErrorCode"] = static_cast<uint8_t>(
                    USBRCMRecoveryErrorCode::ImageTransferFailed);
                return false;
            }

            if (verbose)
            {
                std::cout << std::format("  Image {} sent successfully\n",
                                         i + 1);
                std::cout << std::format(
                    "Monitoring progress after image {}...\n", i + 1);
            }
            ProgressStatus status = monitorProgressAfterImageTransfer(
                rawHandle, lastProcessedTimestamp, errorDetail, errorCode,
                verbose);
            if (status == ProgressStatus::Error)
            {
                // Accept empty-DOT error only when seen right after sending the
                // DOT blob (within the 4s monitor window), not one transfer
                // late.
                const bool isEmptyDotAcceptable =
                    errorCode == USBRCMRecoveryErrorCode::
                                     PscRomMutableDotHeaderCheckFail &&
                    !blobPath.empty() &&
                    (imagePath == blobPath && isEmptyDotBlob(imageData));
                if (isEmptyDotAcceptable)
                {
                    emptyDotBlobAccepted = true;
                    if (verbose)
                    {
                        std::cout << "Empty DOT blob accepted (expected in DOT "
                                     "recovery flow), continuing.\n";
                    }
                }
                else if (emptyDotBlobAccepted)
                {
                    // Recovery failed; we had accepted empty DOT earlier,
                    // report task success and set EmptyDotBlobAccepted so RF
                    // shows the resolution message instead of failure.
                    return reportEmptyDotSoftSuccess();
                }
                else
                {
                    jsonOutput["Status"] = "Failed";
                    jsonOutput["Error"] = errorDetail;
                    jsonOutput["ErrorCode"] = static_cast<uint8_t>(errorCode);
                    return false;
                }
            }
            if (status == ProgressStatus::Completed)
            {
                jsonOutput["Status"] = "Successful";
                return true;
            }

            if (i < finalImagePaths.size() - 1)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(INTER_IMAGE_DELAY_MS));
            }
        }

        // All images sent but no completion code: recovery failed. If we
        // accepted the empty-DOT error, report task success and set flag so
        // Redfish shows the EmptyDotBlobAccepted message with resolution.
        if (emptyDotBlobAccepted)
        {
            return reportEmptyDotSoftSuccess();
        }
        jsonOutput["Status"] = "Successful";
        return true;
    }
    catch (const std::exception& e)
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = std::format("Exception: {}", e.what());
        jsonOutput["ErrorCode"] =
            static_cast<uint8_t>(USBRCMRecoveryErrorCode::ImageTransferFailed);
        return false;
    }
}
