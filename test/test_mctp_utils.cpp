/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../fw-status/mctp_util/utils.hpp"

#include <unistd.h>

#include "../fw-status/mctp_util/utils.cpp"

#include "gtest/gtest.h"

// ========================== printBuffer Tests ==========================

TEST(MctpUtils, PrintBufferTxWithEid)
{
    std::vector<uint8_t> buf = {0x01, 0x02, 0xAB, 0xFF};
    EXPECT_NO_THROW(utils::printBuffer(utils::Tx, buf, 42));
}

TEST(MctpUtils, PrintBufferRxWithEid)
{
    std::vector<uint8_t> buf = {0xDE, 0xAD};
    EXPECT_NO_THROW(utils::printBuffer(utils::Rx, buf, 10));
}

TEST(MctpUtils, PrintBufferEmptyWithEid)
{
    std::vector<uint8_t> buf;
    EXPECT_NO_THROW(utils::printBuffer(utils::Tx, buf, 1));
}

TEST(MctpUtils, PrintBufferTxNoEid)
{
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    EXPECT_NO_THROW(utils::printBuffer(utils::Tx, buf));
}

TEST(MctpUtils, PrintBufferRxNoEid)
{
    std::vector<uint8_t> buf = {0xCA, 0xFE};
    EXPECT_NO_THROW(utils::printBuffer(utils::Rx, buf));
}

TEST(MctpUtils, PrintBufferEmptyNoEid)
{
    std::vector<uint8_t> buf;
    EXPECT_NO_THROW(utils::printBuffer(utils::Rx, buf));
}

// ========================== CustomFD Tests ==========================

TEST(MctpUtils, CustomFDValidFd)
{
    // Create a real fd using pipe
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    close(fds[1]); // close write end

    {
        utils::CustomFD fd(fds[0]);
        EXPECT_EQ(fd(), fds[0]);
    }
    // After destruction, fd should be closed
    // Verify by checking that read fails with EBADF
    char buf;
    EXPECT_EQ(read(fds[0], &buf, 1), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(MctpUtils, CustomFDInvalidFd)
{
    // -1 should not crash on destruction
    {
        utils::CustomFD fd(-1);
        EXPECT_EQ(fd(), -1);
    }
}

TEST(MctpUtils, CustomFDOperator)
{
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    utils::CustomFD fd(fds[0]);
    EXPECT_GE(fd(), 0);
    close(fds[1]);
}
