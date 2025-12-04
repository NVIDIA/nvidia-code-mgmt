/**
 * @file progress_code_queue.cpp
 * @brief Progress code queue reader implementation
 */

#include "progress_code_queue.hpp"
#include "progress_code_parser.hpp"
#include "usb_io.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <type_traits>
#include <libusb-1.0/libusb.h>

using usb_io::readRegister32;

namespace progress_queue {

namespace {

  /**
   * @brief USB Memory Window 3 - Boot Progress Log area
   */
  namespace MemoryWindow {
    constexpr uint32_t WINDOW_3_BASE = 0x8000;   ///< Boot Progress Log base address
    constexpr uint32_t WINDOW_3_SIZE = 0x2000;   ///< Window size: 8kB (8192 bytes)
  }

  /**
   * @brief Progress Code Queue 0 pointer register address
   */
  namespace QueuePointer {
    constexpr uint32_t QUEUE0_OFFSET = 0x2000;   ///< Queue 0 pointer register (SCRATCH_FF_L0_GRP2_0)
  }

  /**
   * @brief Progress Code Queue capacity limits
   */
  namespace Limits {
    constexpr uint32_t MAX_QUEUE_ENTRIES = 1024;  ///< Maximum entries per queue
  }

  /**
   * @brief CPU Progress Code Queue pointers structure
   *
   * Holds the metadata for navigating the circular Progress Code Queue in hardware.
   * These values are parsed from the queue pointer register and define the valid
   * range of entries to read.
   */
  struct CpuPcqPointers {
    uint32_t endIndex;    ///< bits [9:0]   - Next location to be written by hardware
    uint32_t startIndex;  ///< bits [19:10] - Location with oldest valid entry
    uint32_t queueSize;   ///< bits [30:20] - Total queue capacity (1-1024 entries)
  };

  // Queue pointer bit field masks and shifts
  constexpr uint32_t END_INDEX_MASK = 0x3FF;
  constexpr uint32_t START_INDEX_MASK = 0x3FF;
  constexpr uint32_t QUEUE_SIZE_MASK = 0x7FF;
  constexpr uint32_t START_INDEX_SHIFT = 10;
  constexpr uint32_t QUEUE_SIZE_SHIFT = 20;

  // Limits and constraints
  constexpr uint32_t MAX_ITERATION_LIMIT = 2048;

  // Invalid timestamp markers
  constexpr uint32_t INVALID_TIMESTAMP_ZERO = 0x00000000;
  constexpr uint32_t INVALID_TIMESTAMP_MAX = 0xFFFFFFFF;

  // Memory layout constants
  constexpr uint32_t ENTRY_SIZE_BYTES = 8;
  constexpr uint32_t PROGRESS_CODE_OFFSET = 4;

  // Compile-time assertions for type safety and correctness
  static_assert(std::is_standard_layout_v<CpuPcqPointers>,
                "CpuPcqPointers must be standard layout for USB I/O");
  static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
  static_assert(sizeof(uint8_t) == 1, "uint8_t must be 1 byte");

  /**
   * @brief Check if timestamp is invalid (hardware uninitialized or overflow marker)
   */
  [[nodiscard]] constexpr bool isInvalidTimestamp(uint32_t timestamp) noexcept {
    return timestamp == INVALID_TIMESTAMP_ZERO || timestamp == INVALID_TIMESTAMP_MAX;
  }

  /**
   * @brief Advance circular queue index
   */
  [[nodiscard]] constexpr uint32_t advanceCircularIndex(uint32_t currentIdx, uint32_t baseIdx,
                                                          uint32_t queueSize) noexcept {
    return ((currentIdx - baseIdx + 1) % queueSize) + baseIdx;
  }

  /**
   * @brief Parse CPU Progress Code Queue pointers from raw register value
   *
   * @param regValue Raw 32-bit register value containing queue pointer fields
   * @return Parsed queue pointers structure
   *
   * Progress Code Queue pointer bit fields:
   * - End index:   [9:0]   - Next location to be written
   * - Start index: [19:10] - Location with oldest valid code
   * - Size:        [30:20] - Size of the queue (1-1024)
   */
  [[nodiscard]] constexpr CpuPcqPointers parseCpuPcqPointers(uint32_t regValue) noexcept {
    return {
      .endIndex = regValue & END_INDEX_MASK,
      .startIndex = (regValue >> START_INDEX_SHIFT) & START_INDEX_MASK,
      .queueSize = (regValue >> QUEUE_SIZE_SHIFT) & QUEUE_SIZE_MASK
    };
  }

  /**
   * @brief Read CPU Progress Code Queue 0 pointers (internal helper)
   * @param handle USB device handle for communication (must not be null)
   * @param verbose Enable verbose logging
   * @return Parsed queue pointers, or std::nullopt on failure
   */
  [[nodiscard]] std::optional<CpuPcqPointers> readCpuPcqPointers(libusb_device_handle *handle,
                                                                   const bool verbose) {
    if (!handle) {
      if (verbose) {
        std::cerr << "Invalid handle for readCpuPcqPointers\n";
      }
      return std::nullopt;
    }

    auto regValue = readRegister32(handle, QueuePointer::QUEUE0_OFFSET, verbose);

    if (!regValue) {
      if (verbose) {
        std::cerr << "Failed to read CPU PCQ 0 pointers from register 0x"
                  << std::hex << QueuePointer::QUEUE0_OFFSET << std::dec << "\n";
      }
      return std::nullopt;
    }

    auto pointers = parseCpuPcqPointers(*regValue);

    // Validate queueSize is within specification limits
    if (pointers.queueSize > Limits::MAX_QUEUE_ENTRIES) {
      if (verbose) {
        std::cerr << "Invalid queue size: " << pointers.queueSize
                  << " (max: " << Limits::MAX_QUEUE_ENTRIES << ")\n";
      }
      return std::nullopt;
    }

    return pointers;
  }

}  // anonymous namespace

bool readCpuPcqEntries(libusb_device_handle *handle,
                       std::vector<CPUProgressLogEntry> &entries,
                       const bool verbose) {
  if (!handle) {
    if (verbose) {
      std::cerr << "Invalid device handle for readCpuPcqEntries\n";
    }
    return false;
  }

  entries.clear();

  // Read Progress Code Queue 0 pointers
  auto pointers = readCpuPcqPointers(handle, verbose);
  if (!pointers) {
    if (verbose) {
      std::cerr << "Failed to read PCQ 0 pointers\n";
    }
    return false;
  }

  if (pointers->queueSize == 0) {
    if (verbose) {
      std::cout << "PCQ 0 is empty (size=0)\n";
    }
    return true; // Empty queue is not an error
  }

  // Validate queue pointer indices are within bounds
  if (pointers->startIndex >= pointers->queueSize ||
      pointers->endIndex >= pointers->queueSize) {
    if (verbose) {
      std::cerr << "Invalid PCQ pointers from hardware: start="
                << pointers->startIndex << " end=" << pointers->endIndex
                << " size=" << pointers->queueSize << '\n';
    }
    return false;
  }

  // Queue 0 uses zero base index
  constexpr uint32_t qbaseIdx = 0;
  const uint32_t curStart = pointers->startIndex;
  const uint32_t curEnd = pointers->endIndex;

  // For this hardware: write pointer == read pointer means BOTH empty queue AND full queue
  // Disambiguate: if queueSize != 0 and start == end, queue is FULL (not empty)
  const bool isQueueFull = (curStart == curEnd) && (pointers->queueSize != 0);

  // Calculate number of entries to read
  const uint32_t entriesToRead = isQueueFull ? pointers->queueSize
                                              : (curEnd + pointers->queueSize - curStart) % pointers->queueSize;

  if (entriesToRead == 0) {
    if (verbose) {
      std::cout << "PCQ 0 is empty (no entries to read)\n";
    }
    return true; // Empty queue is not an error
  }

  uint32_t readIdx = curStart;

  // Read exactly entriesToRead entries
  for (uint32_t i = 0; i < entriesToRead && i < MAX_ITERATION_LIMIT; ++i) {

    // Calculate memory addresses for this entry (8 bytes per entry)
    // Check for potential overflow in address calculation
    constexpr uint32_t maxSafeIndex = (UINT32_MAX - MemoryWindow::WINDOW_3_BASE) / ENTRY_SIZE_BYTES;
    if (readIdx > maxSafeIndex) {
      if (verbose) {
        std::cerr << "Address overflow detected at index " << readIdx << '\n';
      }
      return false;
    }

    const uint32_t timestampAddr = MemoryWindow::WINDOW_3_BASE + (readIdx * ENTRY_SIZE_BYTES);
    const uint32_t progressCodeAddr = timestampAddr + PROGRESS_CODE_OFFSET;

    // Read timestamp and progress code
    auto timestamp = readRegister32(handle, timestampAddr, verbose);
    auto progressCode = readRegister32(handle, progressCodeAddr, verbose);

    if (!timestamp || !progressCode) {
      if (verbose) {
        std::cerr << "Failed to read entry at index " << readIdx
                  << " (timestamp addr: 0x" << std::hex << timestampAddr
                  << ", progress code addr: 0x" << progressCodeAddr << std::dec << ")\n";
      }
      return false;
    }

    // Skip invalid timestamps
    if (isInvalidTimestamp(*timestamp)) {
      readIdx = advanceCircularIndex(readIdx, qbaseIdx, pointers->queueSize);
      continue;
    }

    // Create and add entry using aggregate initialization
    entries.emplace_back(CPUProgressLogEntry{
      .timestamp = *timestamp,
      .progressCode = *progressCode,
      .queueId = 0  // Queue 0 only
    });

    // Move to next entry
    readIdx = advanceCircularIndex(readIdx, qbaseIdx, pointers->queueSize);
  }

  if (entriesToRead > MAX_ITERATION_LIMIT) {
    if (verbose) {
      std::cerr << "Warning: Truncated read due to iteration limit. Wanted "
                << entriesToRead << " entries, read " << MAX_ITERATION_LIMIT << '\n';
    }
  }

  // Sort entries by timestamp for chronological ordering
  std::sort(entries.begin(), entries.end(),
            [](const CPUProgressLogEntry& a, const CPUProgressLogEntry& b) noexcept {
              return a.timestamp < b.timestamp;
            });

  return true;
}

CPUBootSnapshot getCPUBootSnapshot(const std::vector<CPUProgressLogEntry>& entries) {
  using ProgressCodeParser::getProgressCodeInfo;
  using ProgressCodeParser::CodeType;
  using ProgressCodeParser::SubClass;
  using ProgressCodeParser::PscRomProgressCode;

  CPUBootSnapshot result{};

  // Phase 1: Find the latest BOOT_MODE_SEL_DONE (search from newest to oldest)
  auto bootModeIt = std::find_if(entries.rbegin(), entries.rend(),
    [](const CPUProgressLogEntry& entry) {
      const auto info = getProgressCodeInfo(entry.progressCode);
      return info.type == CodeType::Progress &&
             info.subClass == SubClass::PSC_ROM &&
             info.operation == static_cast<uint8_t>(PscRomProgressCode::BOOT_MODE_SEL_DONE);
    });

  if (bootModeIt != entries.rend()) {
    result.latestBootModeSelDone = bootModeIt->progressCode;
  }

  // If no BOOT_MODE_SEL_DONE found, return empty result
  if (bootModeIt == entries.rend()) {
    return result;
  }

  // Phase 2: From BOOT_MODE_SEL_DONE to newest entry, find latest progress and error codes
  // Note: BOOT_MODE_SEL_DONE itself may qualify as the latest progress code

  // Search from newest entry backwards to BOOT_SEL (inclusive)
  // We need to find where to stop: one position past bootModeIt (towards older entries)
  auto searchEnd = bootModeIt;
  ++searchEnd; // Move to the entry before BOOT_SEL (in time)

  // Find latest progress code (search from newest to BOOT_SEL)
  auto progressIt = std::find_if(entries.rbegin(), searchEnd,
    [](const CPUProgressLogEntry& entry) {
      return getProgressCodeInfo(entry.progressCode).type == CodeType::Progress;
    });

  if (progressIt != searchEnd) {
    result.latestProgressCode = progressIt->progressCode;
  }

  // Find latest error code (search from newest to BOOT_SEL)
  auto errorIt = std::find_if(entries.rbegin(), searchEnd,
    [](const CPUProgressLogEntry& entry) {
      return getProgressCodeInfo(entry.progressCode).type == CodeType::Error;
    });

  if (errorIt != searchEnd) {
    result.latestErrorCode = errorIt->progressCode;
  }

  return result;
}

bool readCpuPcqEntriesAfter(libusb_device_handle *handle,
                            uint32_t afterTimestamp,
                            std::vector<CPUProgressLogEntry> &entries,
                            bool verbose) {
  // Read all entries from hardware
  if (!readCpuPcqEntries(handle, entries, verbose)) {
    return false;
  }
  
  // If afterTimestamp is 0, return all entries (no filtering)
  if (afterTimestamp == 0) {
    return true;
  }
  
  // Filter to only entries with timestamp > afterTimestamp
  // Use erase-remove idiom for efficient in-place filtering
  auto newEnd = std::remove_if(entries.begin(), entries.end(),
                                [afterTimestamp](const CPUProgressLogEntry& entry) noexcept {
                                  return entry.timestamp <= afterTimestamp;
                                });
  entries.erase(newEnd, entries.end());
  
  return true;
}

}  // namespace progress_queue
