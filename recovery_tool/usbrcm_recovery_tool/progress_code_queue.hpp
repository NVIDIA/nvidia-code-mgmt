/**
 * @file progress_code_queue.hpp
 * @brief Progress Code Queue reader for CPU boot progress logs
 * 
 * Provides readCpuPcqEntries() to read boot progress codes from hardware queues via USB.
 * Entries are returned sorted by timestamp for chronological analysis.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <optional>

// Forward declarations
struct libusb_device_handle;

namespace progress_queue {

/**
 * @brief Progress log entry from CPU boot Progress Code Queue
 *
 * Each entry represents a single boot progress event captured from the hardware queue.
 * Entries are timestamped and contain the progress code along with the queue identifier.
 */
struct CPUProgressLogEntry {
  uint32_t timestamp;       ///< Hardware timestamp when progress code was logged
  uint32_t progressCode;    ///< 32-bit progress code value
  uint8_t queueId;          ///< Progress Code Queue identifier: 0 (Queue0) or 1 (Queue1)
};

/**
 * @brief Analysis result containing key progress codes from a log
 *
 * Contains the latest occurrences of specific progress codes of interest:
 * - Boot mode selection done (to check recovery status)
 * - Latest overall progress code
 * - Latest error code (if any errors occurred)
 */
struct CPUBootSnapshot {
  std::optional<uint32_t> latestBootModeSelDone;  ///< Latest PSC_ROM_PC_BOOT_MODE_SEL_DONE code
  std::optional<uint32_t> latestProgressCode;     ///< Latest progress code (any type)
  std::optional<uint32_t> latestErrorCode;        ///< Latest error code (if any)
};

/**
 * @brief Read Progress Code Queue 0 entries from a CPU device
 * @param handle USB device handle (must be already opened)
 * @param entries Output vector (cleared, then populated with entries sorted by timestamp)
 * @param verbose Enable verbose logging (default: false)
 * @return true on success, false on failure
 * 
 * Reads the circular hardware queue, skips invalid timestamps (0x00000000, 0xFFFFFFFF),
 * and returns entries sorted by timestamp (oldest first, latest last).
 * 
 * @note Sorting complexity: O(n log n), typically <100μs for max queue size (1024 entries)
 * @note Caller must open/close the device handle
 */
[[nodiscard]] bool readCpuPcqEntries(libusb_device_handle *handle,
                                     std::vector<CPUProgressLogEntry> &entries,
                                     bool verbose = false);

/**
 * @brief Analyze progress code entries to extract key codes
 * @param entries Vector of progress log entries (typically from readCpuPcqEntries)
 * @return CPUBootSnapshot with latest boot mode selection, progress, and error codes
 * 
 * Scans the entries to find:
 * - Latest PSC_ROM_PC_BOOT_MODE_SEL_DONE (operation=0x02, subclass=PSC_ROM)
 * - Latest progress code overall
 * - Latest error code (if any errors present)
 * 
 * @note Entries should be pre-sorted by timestamp (oldest to newest) for accurate results
 * @note Returns empty optionals if no matching codes are found
 */
[[nodiscard]] CPUBootSnapshot getCPUBootSnapshot(
    const std::vector<CPUProgressLogEntry>& entries);

/**
 * @brief Read Progress Code Queue entries after a specific timestamp
 * @param handle USB device handle (must be already opened)
 * @param afterTimestamp Only return entries with timestamp > afterTimestamp (0 = return all)
 * @param entries Output vector (cleared, then populated with new entries sorted by timestamp)
 * @param verbose Enable verbose logging (default: false)
 * @return true on success, false on failure
 * 
 * This is a convenience function for monitoring that filters out already-seen entries.
 * Internally calls readCpuPcqEntries() and filters by timestamp for efficient delta monitoring.
 * 
 * Use cases:
 * - Recovery monitoring: Track only new codes since last check
 * - Incremental updates: Show progress deltas after each operation
 * - Event detection: Monitor for codes appearing after a specific point
 * 
 * @note Pass afterTimestamp=0 to get all entries (equivalent to readCpuPcqEntries)
 * @note Returned entries are sorted by timestamp (oldest first, latest last)
 * @note Caller must open/close the device handle
 */
[[nodiscard]] bool readCpuPcqEntriesAfter(libusb_device_handle *handle,
                                          uint32_t afterTimestamp,
                                          std::vector<CPUProgressLogEntry> &entries,
                                          bool verbose = false);

}  // namespace progress_queue
