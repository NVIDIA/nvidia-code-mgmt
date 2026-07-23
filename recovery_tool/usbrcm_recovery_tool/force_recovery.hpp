#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * Pin configuration for USB RCM force recovery, sourced from entity-manager.
 *
 * Reset is driven via GPIO when resetPins is non-empty, or via a D-Bus method
 * call when resetPins is empty and the Dbus* fields are set.
 * Strap pins are always driven directly with libgpiod.
 */
struct RecoveryPinConfig
{
    // Reset: GPIO mode when non-empty, D-Bus mode when empty
    std::vector<std::string> resetPins;
    std::string dbusService;
    std::string dbusObject;
    std::string dbusInterface;
    std::string dbusMethod;

    // Strap GPIOs (always direct libgpiod)
    std::vector<std::string> strapPins;
    std::vector<int> strapActiveValues;
    std::vector<int> strapDefaultValues;
};

/**
 * Drive CPUs into USB RCM recovery mode.
 *
 * Sequence: assert all resets → set straps → release all resets.
 * On strap failure the CPUs are left in reset; callers should not proceed.
 */
void forceRecoveryMode(const RecoveryPinConfig& cfg, nlohmann::json& out);

/**
 * Restore strap GPIOs to their default (normal-boot) values.
 * Called after devices confirm recovery mode.
 */
void setGPIODefaultPinStates(const RecoveryPinConfig& cfg, nlohmann::json& out);
