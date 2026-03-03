/**
 * @file perform_rcm_recovery.hpp
 * @brief PerformUSBRecovery command - USB RCM recovery image transfer
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * @brief Perform USB RCM recovery by sending images to device
 *
 * Complete recovery workflow:
 * 1. Validate inputs (portPath and imagePaths must be non-empty)
 * 2. Find device by USB port path
 * 3. Read ECID and determine DOT blob requirement
 * 4. Validate DOT blob availability
 * 5. Send DOT blob + images via synchronous USB bulk transfers
 * 6. Monitor progress codes for 4 seconds after each image
 * 7. Return success if completion detected or all images sent without errors
 *
 * @param portPath USB port path (e.g., "1-1.3", "2-4.1") - must be non-empty
 * @param imagePaths Recovery image file paths - must contain at least one image
 * @param blobPath DOT blob file path (optional, error if required but missing)
 * @param jsonOutput JSON result: {"Status": "Successful"|"Failed", "Error":
 * "..."}
 * @param verbose Enable diagnostic output to stdout/stderr (default: false)
 * @return true on success, false on failure (see jsonOutput["Error"])
 *
 * @par Example Success:
 * @code
 * {
 *   "Status": "Successful"
 * }
 * @endcode
 *
 * @par Example Failure:
 * @code
 * {
 *   "Status": "Failed",
 *   "Error": "DOT blob required but not provided"
 * }
 * @endcode
 *
 * @note Thread-safe (isolated USB context per call)
 * @note Exception-safe (RAII resource management)
 * @note Uses synchronous transfers for simplicity
 */
bool performUsbRecovery(const std::string& portPath,
                        const std::vector<std::string>& imagePaths,
                        const std::string& blobPath, nlohmann::json& jsonOutput,
                        bool verbose = false);
