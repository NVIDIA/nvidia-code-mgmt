#pragma once

#include <nlohmann/json.hpp>

#include <string>

/**
 * @brief Force CPU(s) into USB RCM recovery mode
 *
 * This function executes the GPIO command sequence to configure the specified
 * config type for USB RCM recovery mode:
 *
 * 1. Assert IST_SYS_RST_L (active low reset)
 * 2. Assert CPU_FORCED_RECOVERY_L (active low)
 * 3. Configure CPU_BOOT_DEV_SEL[2:0] = 0b000
 * 4. Configure CPU_RECOVERY_TYPE[1:0] = 0b10 (USB RCM mode)
 * 5. Release IST_SYS_RST_L (reset)
 *
 * The sequence resets the CPU(s) into USB RCM recovery mode.
 *
 * Supported config types:
 * - "c2": Single Board - E5010, E5020, Vera C2 MGX aka P5035
 *         GPIO naming: B0_M1_CPU_* and B0_M1_IST_SYS_RST_L-O
 * - "c1g2": Single CPU/Single Board - PG558 aka Strata single board config
 *           GPIO naming: BRD0_CPU_* and BRD0_IST_SYS_RST_L-O
 * - "c2g4": Dual CPU/Dual Board - PG558 aka Strata dual board config
 *           GPIO naming: BRD0_CPU_*, BRD1_CPU_*, BRD0_IST_SYS_RST_L-O,
 * BRD1_IST_SYS_RST_L-O
 *
 * @param configType Config type string (case-insensitive)
 * @param jsonOutput Output JSON object with per-board operation status
 */
void forceRecoveryMode(const std::string& configType,
                       nlohmann::json& jsonOutput);

/**
 * @brief Set CPU GPIO pin states to the default
 *
 * This function configures the GPIO pins to default states for normal boot.
 * Should be called after the device is confirmed to be in recovery mode
 * so that after update + power cycle, the device boots normally.
 *
 * The GPIO configuration is:
 * 1. Deassert CPU_FORCED_RECOVERY_L (set to 1, not in forced recovery)
 * 2. Configure CPU_BOOT_DEV_SEL[2:0] = 0b000
 * 3. Configure CPU_RECOVERY_TYPE[1:0] = 0b10
 *
 * @param configType Config type string (case-insensitive)
 * @param jsonOutput Output JSON object with per-board operation status
 */
void setGPIODefaultPinStates(const std::string& configType,
                             nlohmann::json& jsonOutput);
