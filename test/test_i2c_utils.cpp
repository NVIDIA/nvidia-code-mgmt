/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../recovery_tool/common/i2c_utils.hpp"

#include <cstdarg>
#include <iostream>
#include <optional>
#include <vector>

#include "gtest/gtest.h"

using namespace recovery_tool::i2c_utils;

extern "C" int __real_ioctl(int fd, unsigned long request, ...);

namespace
{

struct IoctlState
{
    std::optional<int> resultOverride;
    int errnoValue = 0;
    int lastFd = -1;
    unsigned long lastRequest = 0;
    unsigned int lastNmsgs = 0;
    std::vector<i2c_msg> lastMsgs;
} ioctlState;

void resetIoctlState()
{
    ioctlState = {};
}

class ThrowingStreamBuf : public std::streambuf
{
  protected:
    std::streamsize xsputn(const char*, std::streamsize) override
    {
        throw std::runtime_error("fake stderr failure");
    }

    int overflow(int) override
    {
        throw std::runtime_error("fake stderr failure");
    }
};

class StreamFailureGuard
{
  public:
    explicit StreamFailureGuard(std::ostream& stream) :
        stream(stream), oldBuffer(stream.rdbuf()),
        oldExceptions(stream.exceptions())
    {
        stream.rdbuf(&throwingBuffer);
        stream.exceptions(oldExceptions | std::ios::badbit | std::ios::failbit);
    }

    ~StreamFailureGuard()
    {
        stream.exceptions(std::ios::goodbit);
        stream.clear();
        stream.rdbuf(oldBuffer);
        stream.exceptions(oldExceptions);
    }

  private:
    std::ostream& stream;
    std::streambuf* oldBuffer;
    std::ios::iostate oldExceptions;
    ThrowingStreamBuf throwingBuffer;
};

} // namespace

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    auto* payload = va_arg(args, void*);
    va_end(args);

    ioctlState.lastFd = fd;
    ioctlState.lastRequest = request;
    ioctlState.lastNmsgs = 0;
    ioctlState.lastMsgs.clear();

    auto* rdwr = static_cast<i2c_rdwr_ioctl_data*>(payload);
    if (rdwr != nullptr && rdwr->msgs != nullptr)
    {
        ioctlState.lastNmsgs = rdwr->nmsgs;
        ioctlState.lastMsgs.assign(rdwr->msgs, rdwr->msgs + rdwr->nmsgs);
    }

    if (ioctlState.resultOverride.has_value())
    {
        errno = ioctlState.errnoValue;
        return *ioctlState.resultOverride;
    }

    return __real_ioctl(fd, request, payload);
}

class I2CUtilsTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        resetIoctlState();
    }

    void TearDown() override
    {
        resetIoctlState();
    }
};

TEST_F(I2CUtilsTest, SendReadRejectsEmptyReadBuffer)
{
    std::vector<uint8_t> command{0x12};
    std::vector<uint8_t> readData;
    EXPECT_FALSE(sendI2cCmdForRead(-1, 0x42, command, readData, false));
}

TEST_F(I2CUtilsTest, SendReadRejectsEmptyCommandBuffer)
{
    std::vector<uint8_t> command;
    std::vector<uint8_t> readData(4);
    EXPECT_FALSE(sendI2cCmdForRead(-1, 0x42, command, readData, true));
}

TEST_F(I2CUtilsTest, SendReadRejectsEmptyCommandBufferWithoutVerbose)
{
    std::vector<uint8_t> command;
    std::vector<uint8_t> readData(4);
    EXPECT_FALSE(sendI2cCmdForRead(-1, 0x42, command, readData, false));
}

TEST_F(I2CUtilsTest, SendReadReturnsFalseWhenIoctlFails)
{
    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EIO;

    std::vector<uint8_t> command{0x10, 0x20};
    std::vector<uint8_t> readData(3);
    EXPECT_FALSE(sendI2cCmdForRead(9, 0x52, command, readData, true));
    EXPECT_EQ(ioctlState.lastFd, 9);
    EXPECT_EQ(ioctlState.lastRequest, static_cast<unsigned long>(I2C_RDWR));
    ASSERT_EQ(ioctlState.lastNmsgs, 2u);
    EXPECT_EQ(ioctlState.lastMsgs[0].addr, 0x52);
    EXPECT_EQ(ioctlState.lastMsgs[0].flags, 0);
    EXPECT_EQ(ioctlState.lastMsgs[1].addr, 0x52);
    EXPECT_EQ(ioctlState.lastMsgs[1].flags, I2C_M_RD);
}

TEST_F(I2CUtilsTest, SendReadReturnsFalseWhenIoctlFailsWithoutVerbose)
{
    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EIO;

    std::vector<uint8_t> command{0x10, 0x20};
    std::vector<uint8_t> readData(3);
    EXPECT_FALSE(sendI2cCmdForRead(9, 0x52, command, readData, false));
}

TEST_F(I2CUtilsTest, SendReadReturnsTrueOnSuccess)
{
    ioctlState.resultOverride = 0;

    std::vector<uint8_t> command{0xAA, 0x55};
    std::vector<uint8_t> readData(2);
    EXPECT_TRUE(sendI2cCmdForRead(7, 0x6A, command, readData, false));
    ASSERT_EQ(ioctlState.lastNmsgs, 2u);
    EXPECT_EQ(ioctlState.lastMsgs[0].len, command.size());
    EXPECT_EQ(ioctlState.lastMsgs[1].len, readData.size());
}

TEST_F(I2CUtilsTest, SendWriteRejectsEmptyWriteBuffer)
{
    std::vector<uint8_t> writeData;
    EXPECT_FALSE(sendI2cCmdForWrite(-1, 0x44, writeData, true));
}

TEST_F(I2CUtilsTest, SendWriteRejectsEmptyWriteBufferWithoutVerbose)
{
    std::vector<uint8_t> writeData;
    EXPECT_FALSE(sendI2cCmdForWrite(-1, 0x44, writeData, false));
}

TEST_F(I2CUtilsTest, SendWriteReturnsFalseWhenIoctlFails)
{
    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EBUSY;

    std::vector<uint8_t> writeData{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_FALSE(sendI2cCmdForWrite(5, 0x33, writeData, false));
    ASSERT_EQ(ioctlState.lastNmsgs, 1u);
    EXPECT_EQ(ioctlState.lastMsgs[0].addr, 0x33);
    EXPECT_EQ(ioctlState.lastMsgs[0].flags, 0);
    EXPECT_EQ(ioctlState.lastMsgs[0].len, writeData.size());
}

TEST_F(I2CUtilsTest, SendWriteReturnsTrueOnSuccess)
{
    ioctlState.resultOverride = 0;

    std::vector<uint8_t> writeData{0x01, 0x02};
    EXPECT_TRUE(sendI2cCmdForWrite(3, 0x77, writeData, true));
    ASSERT_EQ(ioctlState.lastNmsgs, 1u);
    EXPECT_EQ(ioctlState.lastMsgs[0].buf, writeData.data());
}

TEST_F(I2CUtilsTest, SendWriteReadRejectsEmptyWriteBuffer)
{
    std::vector<uint8_t> writeData;
    std::vector<uint8_t> readData(1);
    EXPECT_FALSE(sendI2cCmdForWriteRead(-1, 0x20, writeData, readData, true));
}

TEST_F(I2CUtilsTest, SendWriteReadRejectsEmptyWriteBufferWithoutVerbose)
{
    std::vector<uint8_t> writeData;
    std::vector<uint8_t> readData(1);
    EXPECT_FALSE(sendI2cCmdForWriteRead(-1, 0x20, writeData, readData, false));
}

TEST_F(I2CUtilsTest, SendWriteReadReturnsFalseWhenIoctlFailsWithoutVerbose)
{
    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = ENXIO;

    std::vector<uint8_t> writeData{0x11, 0x22};
    std::vector<uint8_t> readData(6);
    EXPECT_FALSE(sendI2cCmdForWriteRead(12, 0x5C, writeData, readData, false));
}

TEST_F(I2CUtilsTest, SendWriteReadRejectsEmptyReadBuffer)
{
    std::vector<uint8_t> writeData{0x10};
    std::vector<uint8_t> readData;
    EXPECT_FALSE(sendI2cCmdForWriteRead(-1, 0x20, writeData, readData, false));
}

TEST_F(I2CUtilsTest, SendWriteReadReturnsFalseWhenIoctlFails)
{
    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = ENXIO;

    std::vector<uint8_t> writeData{0x11, 0x22};
    std::vector<uint8_t> readData(6);
    EXPECT_FALSE(sendI2cCmdForWriteRead(12, 0x5C, writeData, readData, true));
    ASSERT_EQ(ioctlState.lastNmsgs, 2u);
    EXPECT_EQ(ioctlState.lastMsgs[0].flags, 0);
    EXPECT_EQ(ioctlState.lastMsgs[1].flags, I2C_M_RD);
}

TEST_F(I2CUtilsTest, SendWriteReadReturnsTrueOnSuccess)
{
    ioctlState.resultOverride = 0;

    std::vector<uint8_t> writeData{0x99};
    std::vector<uint8_t> readData(4);
    EXPECT_TRUE(sendI2cCmdForWriteRead(15, 0x48, writeData, readData, false));
    ASSERT_EQ(ioctlState.lastNmsgs, 2u);
    EXPECT_EQ(ioctlState.lastMsgs[0].addr, 0x48);
    EXPECT_EQ(ioctlState.lastMsgs[1].addr, 0x48);
    EXPECT_EQ(ioctlState.lastMsgs[1].len, readData.size());
}

TEST_F(I2CUtilsTest, VerboseErrorBranchesAreCovered)
{
    std::vector<uint8_t> empty;
    std::vector<uint8_t> command{0x12};
    std::vector<uint8_t> readData(2);

    EXPECT_FALSE(sendI2cCmdForRead(1, 0x42, command, empty, true));
    EXPECT_FALSE(sendI2cCmdForRead(1, 0x42, empty, readData, true));

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EIO;
    EXPECT_FALSE(sendI2cCmdForRead(9, 0x52, command, readData, true));

    ioctlState.resultOverride.reset();
    EXPECT_FALSE(sendI2cCmdForWrite(2, 0x33, empty, true));

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EBUSY;
    std::vector<uint8_t> writeData{0xAA, 0xBB};
    EXPECT_FALSE(sendI2cCmdForWrite(5, 0x33, writeData, true));

    ioctlState.resultOverride.reset();
    EXPECT_FALSE(sendI2cCmdForWriteRead(3, 0x20, empty, readData, true));
    EXPECT_FALSE(sendI2cCmdForWriteRead(3, 0x20, command, empty, true));

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = ENXIO;
    EXPECT_FALSE(sendI2cCmdForWriteRead(12, 0x5C, command, readData, true));
}

TEST_F(I2CUtilsTest, VerboseErrorBranchesPropagateStderrFailures)
{
    std::vector<uint8_t> empty;
    std::vector<uint8_t> command{0x12};
    std::vector<uint8_t> writeData{0xAA, 0xBB};
    std::vector<uint8_t> readData(2);

    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForRead(1, 0x42, command, empty, true),
                     std::runtime_error);
    }
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForRead(1, 0x42, empty, readData, true),
                     std::runtime_error);
    }

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EIO;
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForRead(9, 0x52, command, readData, true),
                     std::runtime_error);
    }

    ioctlState.resultOverride.reset();
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForWrite(2, 0x33, empty, true),
                     std::runtime_error);
    }

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = EBUSY;
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForWrite(5, 0x33, writeData, true),
                     std::runtime_error);
    }

    ioctlState.resultOverride.reset();
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForWriteRead(3, 0x20, empty, readData, true),
                     std::runtime_error);
    }
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForWriteRead(3, 0x20, command, empty, true),
                     std::runtime_error);
    }

    ioctlState.resultOverride = -1;
    ioctlState.errnoValue = ENXIO;
    {
        StreamFailureGuard guard(std::cerr);
        EXPECT_THROW(sendI2cCmdForWriteRead(12, 0x5C, command, readData, true),
                     std::runtime_error);
    }
}
