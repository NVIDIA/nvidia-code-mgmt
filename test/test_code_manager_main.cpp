/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gtest/gtest.h"
#undef FAIL

#include <cerrno>
#include <new>
#include <stdexcept>

namespace code_manager_main_test
{

inline testing::NiceMock<sdbusplus::SdBusMock> sharedSdbusMock;
inline int processExistingImagesResult = 0;
inline int startWatchingResult = 0;
inline int processExistingImagesCalls = 0;
inline int startWatchingCalls = 0;
inline bool failInstallAllocation = false;
inline bool failEraseAllocation = false;

void reset()
{
    processExistingImagesResult = 0;
    startWatchingResult = 0;
    processExistingImagesCalls = 0;
    startWatchingCalls = 0;
    failInstallAllocation = false;
    failEraseAllocation = false;
}

} // namespace code_manager_main_test

namespace sdbusplus::bus
{

inline sdbusplus::bus_t new_default_for_test()
{
    return sdbusplus::get_mocked_new(
        &::code_manager_main_test::sharedSdbusMock);
}

} // namespace sdbusplus::bus

#define BaseController TestBaseController
#define new_default new_default_for_test
#define main code_manager_main_for_test
#include "../src/main.cpp"
#undef main
#undef new_default
#undef BaseController

namespace nvidia::software::updater
{

int TestBaseController::processExistingImages()
{
    ++::code_manager_main_test::processExistingImagesCalls;
    return ::code_manager_main_test::processExistingImagesResult;
}

int TestBaseController::startWatching(sdbusplus::bus_t&, sd_event*)
{
    ++::code_manager_main_test::startWatchingCalls;
    return ::code_manager_main_test::startWatchingResult;
}

} // namespace nvidia::software::updater

namespace code_manager_main_test
{

void runInvalidOptionMain()
{
    optind = 0;
    opterr = 0;
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-z"),
                    nullptr};
    static_cast<void>(code_manager_main_for_test(2, argv));
}

void runUnsupportedUpdaterMain()
{
    optind = 0;
    opterr = 0;
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("Nope"), nullptr};
    static_cast<void>(code_manager_main_for_test(3, argv));
}

void runInstallAllocationFailureMain()
{
    optind = 0;
    opterr = 0;
    failInstallAllocation = true;
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"), nullptr};
    static_cast<void>(code_manager_main_for_test(3, argv));
}

void runEraseAllocationFailureMain()
{
    optind = 0;
    opterr = 0;
    failEraseAllocation = true;
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenErase"), nullptr};
    static_cast<void>(code_manager_main_for_test(3, argv));
}

} // namespace code_manager_main_test

extern "C" void* __real__Znwm(std::size_t size);

extern "C" void* __wrap__Znwm(std::size_t size)
{
    if (code_manager_main_test::failInstallAllocation &&
        size == sizeof(nvidia::software::updater::DebugTokenInstallItemUpdater))
    {
        throw std::bad_alloc();
    }
    if (code_manager_main_test::failEraseAllocation &&
        size == sizeof(nvidia::software::updater::DebugTokenEraseItemUpdater))
    {
        throw std::bad_alloc();
    }

    return __real__Znwm(size);
}

class CodeManagerMainTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        code_manager_main_test::reset();
        testing::Mock::VerifyAndClearExpectations(
            &code_manager_main_test::sharedSdbusMock);
        optind = 0;
        opterr = 0;
    }

    void TearDown() override
    {
        testing::Mock::VerifyAndClearExpectations(
            &code_manager_main_test::sharedSdbusMock);
    }
};

TEST_F(CodeManagerMainTest, InvalidOptionExits)
{
    EXPECT_EXIT(code_manager_main_test::runInvalidOptionMain(),
                ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST_F(CodeManagerMainTest, UnsupportedUpdaterExits)
{
    EXPECT_EXIT(code_manager_main_test::runUnsupportedUpdaterMain(),
                ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST_F(CodeManagerMainTest, DebugTokenInstallReturnsSuccess)
{
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"), nullptr};

    EXPECT_EQ(code_manager_main_for_test(3, argv), 0);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 1);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 1);
}

TEST_F(CodeManagerMainTest, ParsesTargetModelAndNoStripInHarness)
{
    char* argv[] = {const_cast<char*>("code-manager"),
                    const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"),
                    const_cast<char*>("-i"),
                    const_cast<char*>("GPU0"),
                    const_cast<char*>("-m"),
                    const_cast<char*>("HGX"),
                    const_cast<char*>("-n"),
                    nullptr};

    EXPECT_EQ(code_manager_main_for_test(8, argv), 0);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 1);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 1);
}

TEST_F(CodeManagerMainTest, DebugTokenEraseReturnsSuccess)
{
    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenErase"), nullptr};

    EXPECT_EQ(code_manager_main_for_test(3, argv), 0);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 1);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 1);
}

TEST_F(CodeManagerMainTest, RequestNameFailureReturnsMinusOne)
{
    EXPECT_CALL(code_manager_main_test::sharedSdbusMock,
                sd_bus_request_name(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(-EIO));

    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"), nullptr};

    EXPECT_EQ(code_manager_main_for_test(3, argv), -1);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 0);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 0);
}

TEST_F(CodeManagerMainTest, ProcessExistingImagesFailureReturnsMinusOne)
{
    code_manager_main_test::processExistingImagesResult = 1;

    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"), nullptr};

    EXPECT_EQ(code_manager_main_for_test(3, argv), -1);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 1);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 0);
}

TEST_F(CodeManagerMainTest, DebugTokenInstallAllocationFailureExits)
{
    EXPECT_EXIT(code_manager_main_test::runInstallAllocationFailureMain(),
                ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST_F(CodeManagerMainTest, DebugTokenEraseAllocationFailureExits)
{
    EXPECT_EXIT(code_manager_main_test::runEraseAllocationFailureMain(),
                ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST_F(CodeManagerMainTest, StartWatchingFailureReturnsMinusOne)
{
    code_manager_main_test::startWatchingResult = -1;

    char* argv[] = {const_cast<char*>("code-manager"), const_cast<char*>("-u"),
                    const_cast<char*>("DebugTokenInstall"), nullptr};

    EXPECT_EQ(code_manager_main_for_test(3, argv), -1);
    EXPECT_EQ(code_manager_main_test::processExistingImagesCalls, 1);
    EXPECT_EQ(code_manager_main_test::startWatchingCalls, 1);
}
