#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct RecoveryPinConfig
{
    std::vector<std::string> resetPins;
    std::string dbusService;
    std::string dbusObject;
    std::string dbusInterface;
    std::string dbusMethod;
    std::vector<std::string> strapPins;
    std::vector<int> strapActiveValues;
    std::vector<int> strapDefaultValues;
};

namespace test::fw_status_fake_force_recovery
{

inline int forceCallCount{};
inline int defaultCallCount{};
inline nlohmann::json forceResult = {{"Status", "Successful"}};
inline nlohmann::json defaultResult = {{"Status", "Successful"}};

inline void reset()
{
    forceCallCount = 0;
    defaultCallCount = 0;
    forceResult = {{"Status", "Successful"}};
    defaultResult = {{"Status", "Successful"}};
}

} // namespace test::fw_status_fake_force_recovery

inline void forceRecoveryMode(const RecoveryPinConfig& /*cfg*/,
                              nlohmann::json& jsonOutput)
{
    ++test::fw_status_fake_force_recovery::forceCallCount;
    jsonOutput = test::fw_status_fake_force_recovery::forceResult;
}

inline void setGPIODefaultPinStates(const RecoveryPinConfig& /*cfg*/,
                                    nlohmann::json& jsonOutput)
{
    ++test::fw_status_fake_force_recovery::defaultCallCount;
    jsonOutput = test::fw_status_fake_force_recovery::defaultResult;
}
