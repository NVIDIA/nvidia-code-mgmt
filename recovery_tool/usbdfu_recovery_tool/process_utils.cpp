// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "process_utils.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>

namespace usbdfu::process
{

namespace
{

constexpr auto timeoutBinary = "/usr/bin/timeout";
// timeout(1) exit status when the command was killed
constexpr int timeoutExitStatus = 124;
// _exit() status of the child when execvp() fails
constexpr int execFailedStatus = 127;

/**
 * Fork and exec argv under timeout(1) with stdout and stderr redirected to
 * outFd.  Returns the child pid, or -1 if fork failed.  The caller must
 * close its copy of outFd, drain the pipe and reap the child.
 */
pid_t spawn(const std::vector<std::string>& argv, int timeoutSecs,
            int outFd) noexcept
{
    pid_t pid = ::fork();
    if (pid != 0)
    {
        return pid;
    }
    // Both streams go to the pipe: dfu-util reports its failures ("No DFU
    // capable USB device", "Cannot set alternate interface") on stderr and
    // they are the only diagnostics.
    ::dup2(outFd, STDOUT_FILENO);
    ::dup2(outFd, STDERR_FILENO);
    ::close(outFd);
    std::string ts = std::to_string(timeoutSecs) + "s";
    std::vector<const char*> execArgs;
    execArgs.reserve(argv.size() + 3); // timeout, duration, argv..., nullptr
    execArgs.push_back(timeoutBinary);
    execArgs.push_back(ts.c_str());
    for (const auto& a : argv)
    {
        execArgs.push_back(a.c_str());
    }
    execArgs.push_back(nullptr);
    ::execvp(timeoutBinary, const_cast<char* const*>(execArgs.data()));
    ::_exit(execFailedStatus);
}

} // namespace

std::optional<std::string> capture(const std::vector<std::string>& argv,
                                   int timeoutSecs, int* exitCode) noexcept
{
    int fds[2];
    if (::pipe(fds) < 0)
    {
        return std::nullopt;
    }
    pid_t pid = spawn(argv, timeoutSecs, fds[1]);
    ::close(fds[1]);
    if (pid < 0)
    {
        ::close(fds[0]);
        return std::nullopt;
    }

    // Drain to EOF first; reaping before reading would deadlock once the
    // child fills the pipe.
    std::string output;
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
    {
        output.append(buf.data(), static_cast<size_t>(n));
    }
    ::close(fds[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) == timeoutExitStatus ||
        WEXITSTATUS(status) == execFailedStatus)
    {
        return std::nullopt;
    }
    if (exitCode != nullptr)
    {
        *exitCode = WEXITSTATUS(status);
    }
    return output;
}

} // namespace usbdfu::process
