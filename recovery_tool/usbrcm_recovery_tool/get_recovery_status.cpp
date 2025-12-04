/**
 * @file get_recovery_status.cpp
 * @brief GetRecoveryStatus implementation - per-device JSON array output
 */

#include "get_recovery_status.hpp"
#include "usb_device_manager.hpp"
#include "usb_io.hpp"
#include "progress_code_queue.hpp"
#include "progress_code_parser.hpp"
#include "ecid_parser.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <format>
#include <string>
#include <vector>

using json = nlohmann::json;

/**
 * @brief Process a single device and create its JSON entry
 * @param deviceInfo USB device (RAII-managed)
 * @param ctx USB context for device handle
 * @param verbose Enable diagnostic output
 * @return JSON object with device status (always valid, errors in "Error" field)
 */
static json processDevice(const usb::UsbDevice& deviceInfo, 
                          usb::UsbContext& ctx, 
                          bool verbose) {
  json deviceEntry = json::object();
  const std::string& portPath = deviceInfo.getPortPath();
  
  // Initialize all fields with appropriate defaults
  // "Unknown" = error/could not determine, "" = not applicable/empty
  deviceEntry["USB Port Path"] = portPath;
  deviceEntry["Error"] = "";
  deviceEntry["CPU Instance"] = "Unknown";
  deviceEntry["Recovery Status"] = "Unknown";
  deviceEntry["DOT Blob Required"] = "Unknown";
  deviceEntry["Boot Selection Info"] = "";
  deviceEntry["Last Progress Code"] = "";
  deviceEntry["Last Error Code"] = "";

  
  // Open device handle with control interface for status reads
  usb::UsbDeviceHandle deviceHandle(deviceInfo.get(), ctx, usb::INTERFACE_CONTROL);
  if (!deviceHandle.isValid()) {
    if (verbose) {
      std::cerr << std::format("Warning: Failed to open device at port {}\n", portPath);
    }
    deviceEntry["Error"] = "Failed to open USB device";
    return deviceEntry;
  }

  // Read progress codes
  std::vector<progress_queue::CPUProgressLogEntry> entries{};
  if (!progress_queue::readCpuPcqEntries(deviceHandle.get(), entries, verbose)) {
    if (verbose) {
      std::cerr << std::format("Warning: Failed to read progress codes from device at port {}\n", portPath);
    }
    deviceEntry["Error"] = "Failed to read progress codes from device";
    return deviceEntry;
  }

  if (entries.empty()) {
    if (verbose) {
      std::cerr << std::format("Warning: No progress code entries found for device at port {}\n", portPath);
    }
    deviceEntry["Error"] = "No progress code entries found for device";
    return deviceEntry;
  }

  // Get CPU boot snapshot (latest codes from last BOOT_MODE_SEL_DONE onwards)
  auto snapshot = progress_queue::getCPUBootSnapshot(entries);
  
  // Helper lambda to format progress code with hex fallback
  auto formatProgressCode = [](uint32_t code) -> std::string {
    auto info = ProgressCodeParser::getProgressCodeInfo(code);
    // Fallback to hex format if no mapping found
    if (info.name.empty()) {
      return std::format("0x{:08X}", code);
    }
    // Format: hex -> name
    return std::format("0x{:08X} -> {}", code, info.name);
  };
  
  // Check for recovery complete status first (highest priority)
  bool isRecoveryComplete = false;
  if (snapshot.latestProgressCode.has_value()) {
    isRecoveryComplete = ProgressCodeParser::isRecoveryComplete(*snapshot.latestProgressCode);
    deviceEntry["Last Progress Code"] = formatProgressCode(*snapshot.latestProgressCode);
    
    // Extract CPU instance from progress code
    auto progressInfo = ProgressCodeParser::getProgressCodeInfo(*snapshot.latestProgressCode);
    deviceEntry["CPU Instance"] = ProgressCodeParser::packageClassToCpuId(progressInfo.cpuId);

    // If the last progress code matches the recovery complete codes, set recovery status
    if (isRecoveryComplete) {
      deviceEntry["Recovery Status"] = "Recovery Complete";
    }
  }
  
  if (snapshot.latestErrorCode.has_value()) {
    deviceEntry["Last Error Code"] = formatProgressCode(*snapshot.latestErrorCode);
  }
  
  // Determine recovery status from BOOT_MODE_SEL_DONE
  if (snapshot.latestBootModeSelDone.has_value()) {
    auto bootSelInfo = ProgressCodeParser::getProgressCodeInfo(*snapshot.latestBootModeSelDone);
    
    if (bootSelInfo.bootSelectionInfo.has_value()) {
      deviceEntry["Boot Selection Info"] = bootSelInfo.getBootSelectionString();
      
      // Extract CPU instance if not already set
      if (deviceEntry["CPU Instance"] == "Unknown") {
        deviceEntry["CPU Instance"] = ProgressCodeParser::packageClassToCpuId(bootSelInfo.cpuId);
      }
      
      // Determine recovery status based on boot mode (if not already set to Recovery Complete)
      if (deviceEntry["Recovery Status"] == "Unknown" &&
          bootSelInfo.bootSelectionInfo.has_value() &&
          bootSelInfo.bootSelectionInfo->bootMode == ProgressCodeParser::BootMode::Recovery) {
        deviceEntry["Recovery Status"] = "In Recovery";
      } else if (deviceEntry["Recovery Status"] == "Unknown") {
        deviceEntry["Recovery Status"] = "Not in Recovery";
      }
    }
  }
  
  // Cross-validate RCM mode (hardware state) with recovery status from progress codes
  // This detects inconsistent device states that indicate hardware or firmware issues
  const bool isInRcmMode = usb::isDeviceInRcmMode(deviceInfo.get(), verbose);
  const std::string recoveryStatus = deviceEntry["Recovery Status"].get<std::string>();
  const bool isInRecoveryFromProgressCode = (recoveryStatus == "In Recovery" || recoveryStatus == "Recovery Complete");

  if (isInRecoveryFromProgressCode && !isInRcmMode) {
    deviceEntry["Error"] = "Device reports recovery status but is not in RCM mode";
    deviceEntry["Recovery Status"] = "Unknown";
    if (verbose) {
      std::cerr << std::format("Warning: Device at port {} reports recovery status '{}' but lacks RCM endpoints\n",
                               portPath, recoveryStatus);
    }
  } else if (!isInRecoveryFromProgressCode && isInRcmMode) {
    deviceEntry["Error"] = "Device is in RCM mode but reports not in recovery progress code";
    deviceEntry["Recovery Status"] = "Unknown";
    if (verbose) {
      std::cerr << std::format("Warning: Device at port {} has RCM endpoints but reports status '{}'\n",
                               portPath, recoveryStatus);
    }
  }

  // Read ECID for DOT blob requirement using modern usb_io API
  if (auto ecid = usb_io::readDeviceEcid(deviceHandle.get(), verbose)) {
    bool needsDotBlob = EcidParser::isDOTBlobRequired(ecid->data());
    deviceEntry["DOT Blob Required"] = needsDotBlob ? "Yes" : "No";
  } else {
    if (verbose) {
      std::cerr << std::format("Warning: Could not read ECID from device at port {}\n", portPath);
    }
    deviceEntry["DOT Blob Required"] = "Unknown";
  }

  return deviceEntry;
}

/**
 * @brief Get recovery status for all Vera devices (VID:PID=0x0955:0x7410)
 * @param jsonOutput JSON array of device entries, or JSON object with "Error" field
 * @param verbose Enable diagnostic output to stderr
 * @return true on success (including no devices), false on critical failure (USB init only)
 */
bool getRecoveryStatus(json& jsonOutput, bool verbose) {
  // Initialize USB context
  usb::UsbContext ctx;
  if (!ctx.isValid()) {
    std::cerr << "Failed to initialize USB subsystem\n";
    jsonOutput = json::object();
    jsonOutput["Error"] = "Failed to initialize USB subsystem";
    return false;
  }

  // Find all devices with VID:PID
  std::vector<usb::UsbDevice> devices = usb::findDevicesByVidPid(ctx, verbose);
  
  if (devices.empty()) {
    if (verbose) {
      std::cerr << "No USB devices found with Vera VID:PID=0x0955:0x7410\n";
    }
    jsonOutput = json::object();
    jsonOutput["Error"] = "No USB devices found with Vera VID:PID=0x0955:0x7410";
    return true;  // Not a critical error, just no devices
  }

  // Process each device and build JSON array
  jsonOutput = json::array();
  
  for (const auto& device : devices) {
    json deviceEntry = processDevice(device, ctx, verbose);
    jsonOutput.push_back(deviceEntry);
  }

  return true;
}
