#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace test::fw_status_fake_force_recovery
{

inline std::vector<std::string> forceCalls{};
inline std::vector<std::string> defaultCalls{};
inline nlohmann::json forceResult = {{"Status", "Successful"}};
inline nlohmann::json defaultResult = {{"Status", "Successful"}};

inline void reset()
{
    forceCalls.clear();
    defaultCalls.clear();
    forceResult = {{"Status", "Successful"}};
    defaultResult = {{"Status", "Successful"}};
}

} // namespace test::fw_status_fake_force_recovery

inline void forceRecoveryMode(const std::string& configType,
                              nlohmann::json& jsonOutput)
{
    test::fw_status_fake_force_recovery::forceCalls.push_back(configType);
    jsonOutput = test::fw_status_fake_force_recovery::forceResult;
}

inline void setGPIODefaultPinStates(const std::string& configType,
                                    nlohmann::json& jsonOutput)
{
    test::fw_status_fake_force_recovery::defaultCalls.push_back(configType);
    jsonOutput = test::fw_status_fake_force_recovery::defaultResult;
}
