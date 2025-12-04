/**
 * @file get_recovery_status.hpp
 * @brief GetRecoveryStatus command - Vera device recovery status enumeration
 */

#pragma once

#include <nlohmann/json.hpp>

/**
 * @brief Get recovery status for all Vera devices (VID:PID=0x0955:0x7410)
 *
 * Discovers USB devices, reads progress codes, validates RCM mode, determines
 * recovery status and DOT blob requirements. Returns JSON array of device
 * entries.
 *
 * @param jsonOutput JSON array of device entries, or JSON object with "Error"
 * field
 * @param verbose Enable diagnostic output to stderr (default: false)
 * @return true on success (including no devices), false on critical USB init
 * failure
 *
 * @note Thread-safe (isolated USB context per call)
 * @note Exception-safe (RAII resource management)
 * @note Devices with errors included in output (not skipped)
 */
bool getRecoveryStatus(nlohmann::json& jsonOutput, bool verbose = false);
