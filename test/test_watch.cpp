/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gtest/gtest.h"

#define private public
#include "../src/watch.hpp"
#undef private

#include <sys/epoll.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

using namespace nvidia::software::updater;

extern "C" int __real_inotify_init1(int flags);
extern "C" int __real_inotify_add_watch(int fd, const char* pathname,
                                        uint32_t mask);
extern "C" ssize_t __real_read(int fd, void* buf, size_t count);
extern "C" ssize_t __real___read_chk(int fd, void* buf, size_t count,
                                     size_t buflen);
extern "C" int __real_sd_event_add_io(sd_event* event, sd_event_source** source,
                                      int fd, uint32_t events,
                                      sd_event_io_handler_t callback,
                                      void* userdata);

namespace
{

struct WatchWrapState
{
    std::optional<int> inotifyInitResult;
    int inotifyInitErrno = 0;
    std::optional<int> inotifyAddWatchResult;
    int inotifyAddWatchErrno = 0;
    std::optional<int> eventAddIoResult;
    bool forceReadError = false;
    int readErrno = 0;
    std::vector<uint8_t> syntheticRead;
} wrapState;

void resetWrapState()
{
    wrapState = {};
}

std::filesystem::path uniqueWatchDir(const char* suffix)
{
    auto dir = std::filesystem::temp_directory_path() /
               ("code-mgmt-watch-" + std::to_string(::getpid()) + "-" + suffix);
    std::filesystem::remove_all(dir);
    return dir;
}

std::vector<uint8_t> makeEventBuffer(int wd, uint32_t mask,
                                     const std::string& name)
{
    const auto eventSize = offsetof(inotify_event, name) + name.size() + 1;
    std::vector<uint8_t> buffer(eventSize);
    auto* event = reinterpret_cast<inotify_event*>(buffer.data());
    event->wd = wd;
    event->mask = mask;
    event->cookie = 0;
    event->len = name.size() + 1;
    std::memcpy(event->name, name.c_str(), name.size() + 1);
    return buffer;
}

class WatchWrapGuard
{
  public:
    WatchWrapGuard()
    {
        resetWrapState();
    }

    ~WatchWrapGuard()
    {
        resetWrapState();
    }
};

} // namespace

extern "C" int __wrap_inotify_init1(int flags)
{
    if (wrapState.inotifyInitResult.has_value())
    {
        errno = wrapState.inotifyInitErrno;
        return *wrapState.inotifyInitResult;
    }
    return __real_inotify_init1(flags);
}

extern "C" int __wrap_inotify_add_watch(int fd, const char* pathname,
                                        uint32_t mask)
{
    if (wrapState.inotifyAddWatchResult.has_value())
    {
        errno = wrapState.inotifyAddWatchErrno;
        return *wrapState.inotifyAddWatchResult;
    }
    return __real_inotify_add_watch(fd, pathname, mask);
}

extern "C" ssize_t __wrap_read(int fd, void* buf, size_t count)
{
    if (wrapState.forceReadError)
    {
        errno = wrapState.readErrno;
        return -1;
    }
    if (!wrapState.syntheticRead.empty())
    {
        const auto bytes = std::min(count, wrapState.syntheticRead.size());
        std::memcpy(buf, wrapState.syntheticRead.data(), bytes);
        wrapState.syntheticRead.clear();
        return static_cast<ssize_t>(bytes);
    }
    return __real_read(fd, buf, count);
}

extern "C" ssize_t __wrap___read_chk(int fd, void* buf, size_t count,
                                     size_t buflen)
{
    if (wrapState.forceReadError)
    {
        errno = wrapState.readErrno;
        return -1;
    }
    if (!wrapState.syntheticRead.empty())
    {
        const auto bytes = std::min(count, wrapState.syntheticRead.size());
        if (bytes > buflen)
        {
            errno = EOVERFLOW;
            return -1;
        }
        std::memcpy(buf, wrapState.syntheticRead.data(), bytes);
        wrapState.syntheticRead.clear();
        return static_cast<ssize_t>(bytes);
    }
    return __real___read_chk(fd, buf, count, buflen);
}

extern "C" int __wrap_sd_event_add_io(sd_event* event, sd_event_source** source,
                                      int fd, uint32_t events,
                                      sd_event_io_handler_t callback,
                                      void* userdata)
{
    if (wrapState.eventAddIoResult.has_value())
    {
        return *wrapState.eventAddIoResult;
    }
    return __real_sd_event_add_io(event, source, fd, events, callback,
                                  userdata);
}

TEST(WatchTest, CallbackIgnoresNonEpollInEvents)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("ignore");
    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    int callbackCount = 0;
    {
        Watch watch(loop, {dir}, [&](std::string&) {
            callbackCount++;
            return 0;
        });
        EXPECT_EQ(Watch::callback(nullptr, watch.fd, EPOLLERR, &watch), 0);
        EXPECT_EQ(callbackCount, 0);
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, CallbackProcessesClosedFile)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("success");
    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    int callbackCount = 0;
    std::string seenPath;
    {
        Watch watch(loop, {dir}, [&](std::string& imagePath) {
            callbackCount++;
            seenPath = imagePath;
            return 0;
        });

        auto imagePath = dir / "image.bin";
        {
            std::ofstream image(imagePath);
            image << "payload";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(Watch::callback(nullptr, watch.fd, EPOLLIN, &watch), 0);
        EXPECT_EQ(callbackCount, 1);
        EXPECT_EQ(seenPath, imagePath.string());
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, CallbackLogsProcessingFailures)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("failure");
    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    int callbackCount = 0;
    {
        Watch watch(loop, {dir}, [&](std::string&) {
            callbackCount++;
            return -1;
        });

        auto imagePath = dir / "broken.bin";
        {
            std::ofstream image(imagePath);
            image << "payload";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(Watch::callback(nullptr, watch.fd, EPOLLIN, &watch), 0);
        EXPECT_EQ(callbackCount, 1);
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, ConstructorRemovesPreexistingDirectoryContents)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("preexisting");
    std::filesystem::create_directories(dir);
    auto staleFile = dir / "stale.bin";
    {
        std::ofstream stale(staleFile);
        stale << "old";
    }

    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);
    {
        Watch watch(loop, {dir}, [](std::string&) { return 0; });
        EXPECT_TRUE(std::filesystem::exists(dir));
        EXPECT_FALSE(std::filesystem::exists(staleFile));
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, ConstructorThrowsWhenInotifyInitFails)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("initfail");
    wrapState.inotifyInitResult = -1;
    wrapState.inotifyInitErrno = EMFILE;

    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    std::string error;
    try
    {
        Watch watch(loop, {dir}, [](std::string&) { return 0; });
        FAIL() << "expected constructor to throw";
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }
    EXPECT_NE(error.find("inotify_init1 failed"), std::string::npos);

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, ConstructorThrowsWhenAddWatchFails)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("addwatchfail");
    wrapState.inotifyAddWatchResult = -1;
    wrapState.inotifyAddWatchErrno = ENOENT;

    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    std::string error;
    try
    {
        Watch watch(loop, {dir}, [](std::string&) { return 0; });
        FAIL() << "expected constructor to throw";
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }
    EXPECT_NE(error.find("inotify_add_watch failed"), std::string::npos);

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, ConstructorThrowsWhenEventRegistrationFails)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("eventfail");
    wrapState.eventAddIoResult = -EINVAL;

    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    std::string error;
    try
    {
        Watch watch(loop, {dir}, [](std::string&) { return 0; });
        FAIL() << "expected constructor to throw";
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }
    EXPECT_NE(error.find("failed to add to event loop"), std::string::npos);

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, CallbackThrowsOnReadFailure)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("readfail");
    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    {
        Watch watch(loop, {dir}, [](std::string&) { return 0; });
        wrapState.forceReadError = true;
        wrapState.readErrno = EIO;
        EXPECT_THROW(Watch::callback(nullptr, watch.fd, EPOLLIN, &watch),
                     std::runtime_error);
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}

TEST(WatchTest, CallbackIgnoresDirectoryCloseEvents)
{
    WatchWrapGuard wrapGuard;
    auto dir = uniqueWatchDir("dir-event");
    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    int callbackCount = 0;
    {
        Watch watch(loop, {dir}, [&](std::string&) {
            callbackCount++;
            return 0;
        });
        ASSERT_FALSE(watch.wds.empty());
        wrapState.syntheticRead = makeEventBuffer(
            watch.wds.begin()->first, IN_CLOSE_WRITE | IN_ISDIR, "subdir");

        EXPECT_EQ(Watch::callback(nullptr, watch.fd, EPOLLIN, &watch), 0);
        EXPECT_EQ(callbackCount, 0);
    }

    sd_event_unref(loop);
    std::filesystem::remove_all(dir);
}
