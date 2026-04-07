#pragma once

#include <deque>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace test::fw_status_fake_ocp
{

struct StatusReply
{
    bool success = true;
    std::vector<uint8_t> output{};
    std::string error{};
};

inline std::deque<StatusReply> statusReplies{};
inline std::pair<bool, std::string> forceRecoveryResult{
    true,
    "success",
};
inline std::vector<std::tuple<int, int, bool, bool>> constructors{};

inline void reset()
{
    statusReplies.clear();
    forceRecoveryResult = {true, "success"};
    constructors.clear();
}

inline void pushStatusReply(StatusReply reply)
{
    statusReplies.push_back(std::move(reply));
}

} // namespace test::fw_status_fake_ocp

namespace recovery_tool::recovery_commands
{

class OCPRecoveryCommands
{
  public:
    OCPRecoveryCommands(int busAddr, int slaveAddr, bool verbose, bool emul)
    {
        test::fw_status_fake_ocp::constructors.emplace_back(busAddr, slaveAddr,
                                                            verbose, emul);
    }

    std::pair<bool, std::string> setForceRecoveryMode()
    {
        return test::fw_status_fake_ocp::forceRecoveryResult;
    }

    std::tuple<bool, std::vector<uint8_t>, std::string> getDeviceStatusCommand()
    {
        if (test::fw_status_fake_ocp::statusReplies.empty())
        {
            return {true, {0, 0, 0, 0}, ""};
        }

        auto reply = std::move(test::fw_status_fake_ocp::statusReplies.front());
        test::fw_status_fake_ocp::statusReplies.pop_front();
        return {reply.success, std::move(reply.output), std::move(reply.error)};
    }
};

} // namespace recovery_tool::recovery_commands
