#include "force_recovery.hpp"
#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace GpioConfig {
  struct GpioCommand {
    std::string pinName;
    int value;
    std::string description;
  };

  // Config type definitions
  enum class ConfigType {
    C2,    // Board with B0_M1_CPU_* prefix
    C1G2,  // Single CPU with BRD0_CPU_* prefix
    C2G4   // Dual CPU with BRD0_CPU_* and BRD1_CPU_* prefixes
  };

  // Parse config type from string
  std::optional<ConfigType> parseConfigType(const std::string& configStr) noexcept {
    // Make a copy to convert to lowercase (leave original unchanged)
    std::string lower = configStr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "c2") {
      return ConfigType::C2;
    } else if (lower == "c1g2") {
      return ConfigType::C1G2;
    } else if (lower == "c2g4") {
      return ConfigType::C2G4;
    }
    return std::nullopt;
  }

  // Get GPIO prefix for a specific config type and board instance
  std::string getGpioPrefix(ConfigType type, int boardInstance) noexcept {
    switch (type) {
      case ConfigType::C2:
        return "B0_M1_CPU";
      case ConfigType::C1G2:
      case ConfigType::C2G4:
        // Both use BRD<N>_CPU pattern
        return "BRD" + std::to_string(boardInstance) + "_CPU";
    }
    return "";
  }

  // Get reset GPIO prefix for a specific config type and board instance
  std::string getResetPrefix(ConfigType type, int boardInstance) noexcept {
    switch (type) {
      case ConfigType::C2:
        return "B0_M1";
      case ConfigType::C1G2:
      case ConfigType::C2G4:
        // Both use BRD<N> pattern
        return "BRD" + std::to_string(boardInstance);
    }
    return "";
  }

  // Get number of boards for a config type
  int getBoardCount(ConfigType type) noexcept {
    switch (type) {
      case ConfigType::C2:
        return 1;
      case ConfigType::C1G2:
        return 1;
      case ConfigType::C2G4:
        return 2;
    }
    return 0;
  }

  // Get GPIO configuration commands for a specific board
  std::vector<GpioCommand> getGpioConfigSequence(ConfigType type, int boardInstance) {
    std::string prefix = getGpioPrefix(type, boardInstance);
    
    return {
      // Assert forced recovery (active low)
      {prefix + "_FORCED_RECOVERY_L-O", 0, "Assert CPU forced recovery"},
      
      // Configure boot device selection (all 0)
      {prefix + "_BOOT_DEV_SEL0-O", 0, "Boot device select bit 0"},
      {prefix + "_BOOT_DEV_SEL1-O", 0, "Boot device select bit 1"},
      {prefix + "_BOOT_DEV_SEL2-O", 0, "Boot device select bit 2"},
      
      // Configure recovery type (TYPE0=0, TYPE1=1 for USB RCM)
      {prefix + "_RECOVERY_TYPE0-O", 0, "Recovery type bit 0"},
      {prefix + "_RECOVERY_TYPE1-O", 1, "Recovery type bit 1 (USB RCM)"},
    };
  }

  // Common reset commands (same for all configs)
  GpioCommand getResetAssert(ConfigType type, int boardInstance) noexcept {
    std::string prefix = getResetPrefix(type, boardInstance);
    return {prefix + "_IST_SYS_RST_L-O", 0, "Assert system reset"};
  }

  GpioCommand getResetRelease(ConfigType type, int boardInstance) noexcept {
    std::string prefix = getResetPrefix(type, boardInstance);
    return {prefix + "_IST_SYS_RST_L-O", 1, "Release system reset"};
  }
}

/**
 * @brief Validate GPIO pin name to prevent shell injection
 * @param pinName The pin name to validate
 * @return true if pin name is safe, false otherwise
 */
static bool isValidPinName(const std::string &pinName) noexcept {
  if (pinName.empty()) {
    return false;
  }
  
  // Only allow alphanumeric, underscore, and hyphen characters
  return std::all_of(pinName.begin(), pinName.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  });
}

/**
 * @brief Execute a shell command with timeout and check exit status
 * @param command The command to execute
 * @return true if command succeeded (exit code 0), false otherwise
 * 
 * @note Commands are wrapped with 'timeout 5s' to prevent indefinite hangs
 *       from kernel driver issues, hardware faults, or bus contention
 */
static bool executeCommand(const std::string &command) noexcept {
  // Wrap command with timeout to prevent indefinite hangs (e.g., kernel driver issues)
  // Redirect both stdout and stderr to /dev/null to avoid corrupting JSON output
  std::string fullCommand = "timeout 5s " + command + " >/dev/null 2>&1";
  int exitCode = system(fullCommand.c_str());
  
  if (WIFEXITED(exitCode)) {
    return WEXITSTATUS(exitCode) == 0;
  }
  
  return false;
}

static bool setGpio(const std::string &pinName, int value) {
  // Validate pin name to prevent shell injection
  if (!isValidPinName(pinName)) {
    return false;
  }
  
  std::string command = "gpioset `gpiofind " + pinName + "`=" + std::to_string(value);
  return executeCommand(command);
}

/**
 * @brief Configure recovery mode for a single board instance
 * @param configType Config type (C2, C1G2, or C2G4)
 * @param boardInstance Board instance number (0 or 1)
 * @param jsonOutput Output JSON object with board operation status
 */
static void forceRecoveryModeBoardInstance(GpioConfig::ConfigType configType,
                                           int boardInstance,
                                           nlohmann::json &jsonOutput) {
  jsonOutput.clear();

  // Step 1: Assert reset for this board
  auto reset = GpioConfig::getResetAssert(configType, boardInstance);
  if (!setGpio(reset.pinName, reset.value)) {
    jsonOutput["Status"] = "Failed";
    jsonOutput["Error"] = "Failed to assert reset for board " + std::to_string(boardInstance);
    return;
  }

  // Step 2-4: Configure GPIOs for this board
  bool boardSuccess = true;
  std::string boardError;
  auto configSeq = GpioConfig::getGpioConfigSequence(configType, boardInstance);
  for (const auto &cmd : configSeq) {
    if (!setGpio(cmd.pinName, cmd.value)) {
      boardSuccess = false;
      boardError = cmd.description;
      break;
    }
  }

  // Step 5: Release reset for this board
  auto release = GpioConfig::getResetRelease(configType, boardInstance);
  if (!setGpio(release.pinName, release.value)) {
    boardSuccess = false;
    // Only update error message if not already set (preserve first failure)
    if (boardError.empty()) {
      boardError = release.description;
    }
  }

  // Output JSON
  jsonOutput["Status"] = boardSuccess ? "Successful" : "Failed";
  if (!boardSuccess && !boardError.empty()) {
    jsonOutput["Error"] = boardError;
  }
}

void forceRecoveryMode(const std::string& configTypeStr, nlohmann::json &jsonOutput) {
  jsonOutput.clear();

  // Parse config type
  auto configType = GpioConfig::parseConfigType(configTypeStr);
  if (!configType) {
    jsonOutput["Status"] = "Failed";
    jsonOutput["Error"] = "Invalid config type. Supported: c2, c1g2, c2g4";
    return;
  }

  // Get board count for this config type
  const int boardCount = GpioConfig::getBoardCount(*configType);

  // Configure recovery mode for all boards
  bool allSuccess = true;
  for (int i = 0; i < boardCount; ++i) {
    nlohmann::json boardResult;
    forceRecoveryModeBoardInstance(*configType, i, boardResult);
    
    std::string boardKey = "Board" + std::to_string(i);
    jsonOutput[boardKey] = boardResult;
    
    // Track overall success
    if (boardResult.contains("Status") && boardResult["Status"] == "Failed") {
      allSuccess = false;
    }
  }
  
  // Set top-level status
  jsonOutput["Status"] = allSuccess ? "Successful" : "Failed";
}
