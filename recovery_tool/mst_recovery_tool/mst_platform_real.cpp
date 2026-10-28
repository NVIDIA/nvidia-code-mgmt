// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "mst_platform_real.hpp"

#include "i2c_utils.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace mst
{

namespace
{

constexpr auto consumer = "mst-recovery";
constexpr auto usbSysfsRoot = "/sys/bus/usb/devices/";

std::string dbusError(const std::exception& e)
{
    if (const auto* sd = dynamic_cast<const sdbusplus::exception_t*>(&e))
    {
        return sd->description();
    }
    return e.what();
}

} // namespace

RealPlatform::RealPlatform() : bus(sdbusplus::bus::new_default_system())
{}

std::expected<Value, std::string>
    RealPlatform::getProperty(const std::string& service,
                              const std::string& path, const std::string& iface,
                              const std::string& prop)
{
    std::lock_guard lock(busMutex);
    try
    {
        auto m = bus.new_method_call(service.c_str(), path.c_str(),
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append(iface, prop);
        auto reply = bus.call(m);
        Value value;
        reply.read(value);
        return value;
    }
    catch (const std::exception& e)
    {
        return std::unexpected(dbusError(e));
    }
}

std::expected<PropertyMap, std::string>
    RealPlatform::getAll(const std::string& service, const std::string& path,
                         const std::string& iface)
{
    std::lock_guard lock(busMutex);
    try
    {
        auto m =
            bus.new_method_call(service.c_str(), path.c_str(),
                                "org.freedesktop.DBus.Properties", "GetAll");
        m.append(iface);
        auto reply = bus.call(m);
        PropertyMap props;
        reply.read(props);
        return props;
    }
    catch (const std::exception& e)
    {
        return std::unexpected(dbusError(e));
    }
}

std::expected<std::vector<std::string>, std::string>
    RealPlatform::getSubTreePaths(const std::string& root, int32_t depth,
                                  const std::vector<std::string>& ifaces)
{
    std::lock_guard lock(busMutex);
    try
    {
        auto m = bus.new_method_call("xyz.openbmc_project.ObjectMapper",
                                     "/xyz/openbmc_project/object_mapper",
                                     "xyz.openbmc_project.ObjectMapper",
                                     "GetSubTreePaths");
        m.append(root, depth, ifaces);
        auto reply = bus.call(m);
        std::vector<std::string> paths;
        reply.read(paths);
        return paths;
    }
    catch (const std::exception& e)
    {
        return std::unexpected(dbusError(e));
    }
}

void RealPlatform::createLog(const std::string& message,
                             const std::string& level,
                             const std::map<std::string, std::string>& data)
{
    std::lock_guard lock(busMutex);
    try
    {
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");
        m.append(message, level, data);
        bus.call_noreply(m);
    }
    catch (const std::exception& e)
    {
        lg2::error("Logging.Create {EVENT} failed: {ERROR}", "EVENT", message,
                   "ERROR", dbusError(e));
    }
}

DevInfo RealPlatform::statDevice(const std::string& path)
{
    DevInfo info;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
    {
        return info;
    }
    info.exists = true;
    info.isChar = S_ISCHR(st.st_mode);
    info.readable = access(path.c_str(), R_OK) == 0;
    info.writable = access(path.c_str(), W_OK) == 0;
    return info;
}

bool RealPlatform::isReadableFile(const std::string& path, bool regularOnly)
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
    {
        return false;
    }
    if (regularOnly && !S_ISREG(st.st_mode))
    {
        return false;
    }
    return access(path.c_str(), R_OK) == 0;
}

std::optional<uint64_t> RealPlatform::fileSize(const std::string& path)
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
    {
        return std::nullopt;
    }
    return static_cast<uint64_t>(st.st_size);
}

std::optional<std::array<uint8_t, 4>>
    RealPlatform::i2cWriteRead(const std::string& busNum, uint16_t addr,
                               const std::vector<uint8_t>& write, bool force)
{
    auto path = "/dev/i2c-" + busNum;
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return std::nullopt;
    }
    // Without -f, i2ctransfer refuses an address a kernel driver owns.
    if (!force && ioctl(fd, I2C_SLAVE, addr) < 0)
    {
        close(fd);
        return std::nullopt;
    }
    std::vector<uint8_t> w(write);
    std::vector<uint8_t> r(4);
    bool ok =
        recovery_tool::i2c_utils::sendI2cCmdForWriteRead(fd, addr, w, r, false);
    close(fd);
    if (!ok)
    {
        return std::nullopt;
    }
    return std::array<uint8_t, 4>{r[0], r[1], r[2], r[3]};
}

bool RealPlatform::gpioFind(const std::string& name)
{
    try
    {
        return static_cast<bool>(gpiod::find_line(name));
    }
    catch (const std::exception& e)
    {
        lg2::error("GPIO {NAME}: {ERROR}", "NAME", name, "ERROR", e.what());
        return false;
    }
}

bool RealPlatform::gpioSet(const std::string& name, int value)
{
    std::lock_guard lock(gpioMutex);
    try
    {
        auto line = gpiod::find_line(name);
        if (!line)
        {
            return false;
        }
        // gpioset (libgpiod v1) requests, drives and releases the line.
        line.request({consumer, gpiod::line_request::DIRECTION_OUTPUT, 0},
                     value);
        line.release();
        return true;
    }
    catch (const std::exception& e)
    {
        lg2::error("GPIO {NAME}: {ERROR}", "NAME", name, "ERROR", e.what());
        return false;
    }
}

bool RealPlatform::gpioHold(const std::string& name, int value)
{
    std::lock_guard lock(gpioMutex);
    try
    {
        auto line = gpiod::find_line(name);
        if (!line)
        {
            return false;
        }
        line.request({consumer, gpiod::line_request::DIRECTION_OUTPUT, 0},
                     value);
        held[name] = line;
        return true;
    }
    catch (const std::exception& e)
    {
        lg2::error("GPIO {NAME}: {ERROR}", "NAME", name, "ERROR", e.what());
        return false;
    }
}

void RealPlatform::gpioRelease(const std::string& name)
{
    std::lock_guard lock(gpioMutex);
    auto it = held.find(name);
    if (it == held.end())
    {
        return;
    }
    try
    {
        it->second.release();
    }
    catch (const std::exception& e)
    {
        lg2::error("GPIO {NAME}: {ERROR}", "NAME", name, "ERROR", e.what());
    }
    held.erase(it);
}

std::optional<std::string> RealPlatform::readLine(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(in, line) || in.eof())
    {
        // `read -r` fails when the line has no terminating newline.
        return std::nullopt;
    }
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return std::string{};
    }
    auto last = line.find_last_not_of(" \t");
    return line.substr(first, last - first + 1);
}

std::optional<std::string> RealPlatform::usbAttribute(const std::string& port,
                                                      const std::string& attr)
{
    return readLine(usbSysfsRoot + port + "/" + attr);
}

bool RealPlatform::commandExists(const std::string& name)
{
    const char* env = std::getenv("PATH");
    std::stringstream path(env != nullptr ? env : "");
    std::string dir;
    while (std::getline(path, dir, ':'))
    {
        auto candidate = (dir.empty() ? "." : dir) + "/" + name;
        struct stat st{};
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            access(candidate.c_str(), X_OK) == 0)
        {
            return true;
        }
    }
    return false;
}

int RealPlatform::runProcess(const std::vector<std::string>& argv,
                             OutputMode mode,
                             const std::function<void(std::string_view)>& out)
{
    int fds[2] = {-1, -1};
    if (mode == OutputMode::Merged && pipe2(fds, O_CLOEXEC) != 0)
    {
        return 127;
    }
    std::vector<char*> args;
    for (const auto& a : argv)
    {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);
    {
        std::lock_guard lock(outMutex);
        std::fflush(stdout);
        std::fflush(stderr);
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        if (fds[0] >= 0)
        {
            close(fds[0]);
            close(fds[1]);
        }
        return 127;
    }
    if (pid == 0)
    {
        if (mode == OutputMode::Merged)
        {
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
        }
        execvp(args[0], args.data());
        _exit(127);
    }
    // Only the SIGTERM/SIGINT cancellation handlers interrupt these
    // calls; pass the cancellation on so the burn stops and is reaped
    // before the caller restores the GPIOs.
    bool forwarded = false;
    auto forward = [&]() {
        if (!forwarded)
        {
            kill(pid, SIGTERM);
            forwarded = true;
        }
    };
    if (mode == OutputMode::Merged)
    {
        close(fds[1]);
        char buf[4096];
        while (true)
        {
            auto n = read(fds[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
            {
                forward();
                continue;
            }
            if (n <= 0)
            {
                break;
            }
            if (out)
            {
                out(std::string_view(buf, static_cast<size_t>(n)));
            }
        }
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            return 127;
        }
        forward();
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return 128 + WTERMSIG(status);
}

void RealPlatform::sleepMs(uint64_t ms)
{
    struct timespec ts{static_cast<time_t>(ms / 1000),
                       static_cast<long>((ms % 1000) * 1000000)};
    // A signal ends the sleep early, as it ends the scripts' sleep(1).
    nanosleep(&ts, nullptr);
}

int64_t RealPlatform::nowSeconds()
{
    return static_cast<int64_t>(std::time(nullptr));
}

void RealPlatform::writeOut(std::string_view line)
{
    std::lock_guard lock(outMutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void RealPlatform::writeErr(std::string_view line)
{
    std::lock_guard lock(outMutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace mst
