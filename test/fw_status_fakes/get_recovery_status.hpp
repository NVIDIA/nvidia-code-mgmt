#pragma once

#include <nlohmann/json.hpp>

#include <deque>

namespace test::fw_status_fake_usb_recovery
{

struct StatusReply
{
    bool success = true;
    nlohmann::json payload = nlohmann::json::array();
};

inline std::deque<StatusReply> replies{};
inline bool fallbackSuccess = true;
inline nlohmann::json fallbackPayload = nlohmann::json::array();

inline void reset()
{
    replies.clear();
    fallbackSuccess = true;
    fallbackPayload = nlohmann::json::array();
}

inline void pushReply(bool success, nlohmann::json payload)
{
    replies.push_back({success, std::move(payload)});
}

} // namespace test::fw_status_fake_usb_recovery

inline bool getRecoveryStatus(nlohmann::json& jsonOutput, bool)
{
    using namespace test::fw_status_fake_usb_recovery;

    if (!replies.empty())
    {
        auto reply = std::move(replies.front());
        replies.pop_front();
        jsonOutput = std::move(reply.payload);
        return reply.success;
    }

    jsonOutput = fallbackPayload;
    return fallbackSuccess;
}
