#pragma once

#include <cstdint>
#include <deque>
#include <tuple>

namespace test::fw_status_fake_glacier
{

inline std::deque<uint8_t> initResults{};
inline std::vector<std::tuple<uint64_t, uint64_t, bool>> constructors{};

inline void reset()
{
    initResults.clear();
    constructors.clear();
}

inline void pushResult(uint8_t result)
{
    initResults.push_back(result);
}

} // namespace test::fw_status_fake_glacier

namespace glacier_recovery_tool::glacier_recovery_commands
{

enum class RecoveryResult : uint8_t
{
    Ok = 0x0,
    FirmwareNotInRecovery = 0x8,
    FailedToReadData = 0xD,
};

class GlacierRecoveryCommands
{
  public:
    GlacierRecoveryCommands(uint64_t i2cBus, uint64_t i2cAddress, bool verbose)
    {
        test::fw_status_fake_glacier::constructors.emplace_back(
            i2cBus, i2cAddress, verbose);
    }

    RecoveryResult performInitialization()
    {
        if (test::fw_status_fake_glacier::initResults.empty())
        {
            return RecoveryResult::FirmwareNotInRecovery;
        }

        auto result = test::fw_status_fake_glacier::initResults.front();
        test::fw_status_fake_glacier::initResults.pop_front();
        return static_cast<RecoveryResult>(result);
    }
};

} // namespace glacier_recovery_tool::glacier_recovery_commands
