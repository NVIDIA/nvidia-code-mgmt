#pragma once

#include "handler.hpp"
#include "mctp_endpoint_discovery.hpp"

#include <sdeventplus/event.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace test::fw_status_fake_vdm
{

inline std::vector<uint8_t> bootStatusPayload{};
inline int queryCalls = 0;
inline bool returnResponse = true;
inline bool suspendQuery = false;

inline void reset()
{
    bootStatusPayload.clear();
    queryCalls = 0;
    returnResponse = true;
    suspendQuery = false;
}

} // namespace test::fw_status_fake_vdm

class MCTPVdmHelper : public mctp_vdm::MctpDiscoveryHandlerIntf
{
  public:
    template <typename... Args>
    explicit MCTPVdmHelper(Args&&...)
    {}

    mctp_vdm::requester::Coroutine
        queryBootStatus(uint8_t, const mctp_vdm::Message*& responseMsg,
                        size_t& responseLen)
    {
        ++test::fw_status_fake_vdm::queryCalls;
        static std::vector<uint8_t> storage;

        if (test::fw_status_fake_vdm::suspendQuery)
        {
            co_await std::suspend_always{};
        }

        if (!test::fw_status_fake_vdm::returnResponse ||
            test::fw_status_fake_vdm::bootStatusPayload.empty())
        {
            responseMsg = nullptr;
            responseLen = 0;
            co_return 0;
        }

        storage.resize(sizeof(mctp_vdm::MsgHeader) +
                       test::fw_status_fake_vdm::bootStatusPayload.size());
        auto* msg = reinterpret_cast<mctp_vdm::Message*>(storage.data());
        msg->hdr = {};
        std::copy(test::fw_status_fake_vdm::bootStatusPayload.begin(),
                  test::fw_status_fake_vdm::bootStatusPayload.end(),
                  msg->payload);
        responseMsg = msg;
        responseLen = storage.size() - sizeof(mctp_vdm::MsgHeader);
        co_return 0;
    }

    void handleMctpEndpoints([[maybe_unused]] const mctp::Infos&) override
    {}
};
