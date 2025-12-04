/**
 * @file perform_rcm_recovery.cpp
 * @brief PerformUSBRecovery implementation - USB RCM image transfer via libusb
 */

#include "perform_rcm_recovery.hpp"

#include "ecid_parser.hpp"
#include "progress_code_parser.hpp"
#include "progress_code_queue.hpp"
#include "usb_device_manager.hpp"
#include "usb_io.hpp"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
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
    4; ///< Monitor for 2 seconds after image (4 × 500ms)
constexpr int INTER_IMAGE_DELAY_MS = 250; ///< Pause between images

/// Progress check result status
enum class ProgressStatus
{
    Continue,  ///< No error, no completion
    Completed, ///< Recovery completed
    Error      ///< Error detected
};

// Read file into memory buffer
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
 *
 * @return true if all data sent successfully, false on transfer failure
 */
[[nodiscard]] bool sendBulkData(libusb_device_handle* handle,
                                const uint8_t* data, size_t size, bool verbose)
{
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
            if (verbose)
            {
                std::cerr << std::format(
                    "Bulk transfer failed: {} ({})\n", libusb_error_name(ret),
                    libusb_strerror(static_cast<libusb_error>(ret)));
            }
            return false;
        }

        // Defensive check: ensure progress is made
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
 * @param verbose Enable diagnostic output to stdout/stderr
 *
 * @return ProgressStatus::Completed if recovery completion code detected
 * @return ProgressStatus::Error if error code detected
 * @return ProgressStatus::Continue if no completion or error found
 */
[[nodiscard]] ProgressStatus
    checkForNewProgressCodes(libusb_device_handle* handle,
                             uint32_t& lastProcessedTimestamp,
                             std::string& errorDetail, bool verbose)
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
            return ProgressStatus::Error;
        }

        lastProcessedTimestamp = entry.timestamp;
    }

    return ProgressStatus::Continue;
}

/**
 * @brief Monitor progress codes for 2 seconds after sending an image
 *
 * Polls for new progress codes every 500ms for a total of 2 seconds (4
 * iterations). Returns immediately if completion code or error is detected
 * during monitoring.
 *
 * @param handle USB device handle for reading progress codes
 * @param lastProcessedTimestamp Reference to timestamp of last processed code;
 * updated as new codes are processed
 * @param errorDetail Reference to error message string; populated if error code
 * detected
 * @param verbose Enable diagnostic output to stdout/stderr
 *
 * @return ProgressStatus::Completed if recovery completion detected
 * @return ProgressStatus::Error if error code detected
 * @return ProgressStatus::Continue if monitoring period elapses without
 * completion/error
 */
[[nodiscard]] ProgressStatus
    monitorProgressAfterImageTransfer(libusb_device_handle* handle,
                                      uint32_t& lastProcessedTimestamp,
                                      std::string& errorDetail, bool verbose)
{
    using namespace std::chrono;

    for (int i = 0; i < POST_IMAGE_MONITOR_ITERATIONS; ++i)
    {
        std::this_thread::sleep_for(milliseconds(PROGRESS_CHECK_INTERVAL_MS));

        ProgressStatus status = checkForNewProgressCodes(
            handle, lastProcessedTimestamp, errorDetail, verbose);
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

    // Validate inputs
    if (portPath.empty())
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = "Port path cannot be empty";
        return false;
    }

    if (imagePaths.empty())
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = "At least one recovery image must be provided";
        return false;
    }

    try
    {
        usb::UsbContext ctx;
        if (!ctx.isValid())
        {
            jsonOutput["Status"] = "Failed";
            jsonOutput["Error"] = "Failed to initialize USB subsystem";
            return false;
        }

        auto device = usb::findDeviceByPortPath(ctx, portPath, verbose);
        if (!device)
        {
            jsonOutput["Status"] = "Failed";
            jsonOutput["Error"] =
                std::format("Device not found at port path: {}", portPath);
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
            return false;
        }

        libusb_device_handle* rawHandle = deviceHandle.get();

        if (verbose)
        {
            std::cout << std::format(
                "Opened device at port {} and claimed RCM interface {}\n",
                portPath, usb::INTERFACE_RECOVERY);
        }

        // Determine images to send (DOT blob + recovery images if needed)
        std::vector<std::string> finalImagePaths;

        if (auto ecid = usb_io::readDeviceEcid(rawHandle, verbose))
        {
            const auto blobReq = EcidParser::getBlobRequirement(ecid->data());

            if (blobReq == EcidParser::BlobRequirement::S2ABlob)
            {
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] = "S2A blob required - not supported";
                return false;
            }

            if (blobReq == EcidParser::BlobRequirement::DOTBlob)
            {
                if (blobPath.empty())
                {
                    jsonOutput["Status"] = "Failed";
                    jsonOutput["Error"] = "DOT blob required but not provided";
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
                return false;
            }

            if (verbose)
            {
                std::cout << std::format("  Size: {} bytes\n",
                                         imageData.size());
            }

            if (!sendBulkData(rawHandle, imageData.data(), imageData.size(),
                              verbose))
            {
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] =
                    std::format("Failed to send image: {}", imagePath);
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
                rawHandle, lastProcessedTimestamp, errorDetail, verbose);
            if (status == ProgressStatus::Error)
            {
                jsonOutput["Status"] = "Failed";
                jsonOutput["Error"] = errorDetail;
                return false;
            }
            if (status == ProgressStatus::Completed)
            {
                jsonOutput["Status"] = "Successful";
                return true;
            }

            // Brief pause between images
            if (i < finalImagePaths.size() - 1)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(INTER_IMAGE_DELAY_MS));
            }
        }

        // All images sent successfully, no errors detected during monitoring
        jsonOutput["Status"] = "Successful";
        return true;
    }
    catch (const std::exception& e)
    {
        jsonOutput["Status"] = "Failed";
        jsonOutput["Error"] = std::format("Exception: {}", e.what());
        return false;
    }
}
