#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test::fw_status_fake_i2c
{

struct Reply
{
    bool success = true;
    bool throwException = false;
    std::vector<uint8_t> readData{};
    std::string error = "fake i2c failure";
};

inline std::deque<Reply> replies{};
inline std::vector<uint16_t> slaveAddresses{};
inline std::vector<std::vector<uint8_t>> writeBuffers{};

inline void reset()
{
    replies.clear();
    slaveAddresses.clear();
    writeBuffers.clear();
}

inline void pushReply(Reply reply)
{
    replies.push_back(std::move(reply));
}

} // namespace test::fw_status_fake_i2c

namespace recovery_tool::i2c_utils
{

inline bool sendI2cCmdForWriteRead(int, uint16_t slaveAddr,
                                   const std::vector<uint8_t>& writeData,
                                   std::vector<uint8_t>& readData, bool)
{
    using namespace test::fw_status_fake_i2c;

    slaveAddresses.push_back(slaveAddr);
    writeBuffers.push_back(writeData);

    if (replies.empty())
    {
        std::fill(readData.begin(), readData.end(), 0);
        return true;
    }

    auto reply = std::move(replies.front());
    replies.pop_front();

    if (reply.throwException)
    {
        throw std::runtime_error(reply.error);
    }

    if (!reply.readData.empty())
    {
        readData = std::move(reply.readData);
    }
    return reply.success;
}

} // namespace recovery_tool::i2c_utils
