/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "binary_installer.hpp"

#include "cak.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Virtual override
// ---------------------------------------------------------------------------

asio::awaitable<void> BinaryInstaller::doInstall(const json& payload)
{
    std::string cakBytes = extractCakFromJson(payload.dump());
    fs::path tempKeyFile = config_.keyStorePath / "cak_temp.pem";
    atomicWrite(tempKeyFile, cakBytes);
    try
    {
        co_await runHelper(tempKeyFile);
    }
    catch (...)
    {
        fs::remove(tempKeyFile);
        throw;
    }
    fs::remove(tempKeyFile);
}

// ---------------------------------------------------------------------------
// Helper executable
// ---------------------------------------------------------------------------

asio::awaitable<void> BinaryInstaller::runHelper(const fs::path& cakFile)
{
    if (!config_.hmclessExec)
    {
        throw std::runtime_error("hmclessExec is not configured");
    }
    std::vector<std::string> command;
    command.push_back(config_.hmclessExec->path);
    for (const auto& arg : config_.hmclessExec->args)
    {
        command.push_back(arg);
    }
    command.push_back(cakFile.string());

    RunResult result = co_await execute(command);
    if (result.code != 0)
    {
        throw std::runtime_error("hmcless exec failed: " + result.stderrStr);
    }
}

asio::awaitable<RunResult>
    BinaryInstaller::execute(const std::vector<std::string>& command)
{
    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};
    if (::pipe(stdoutPipe) < 0 || ::pipe(stderrPipe) < 0)
    {
        throw std::runtime_error("pipe failed");
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0)
    {
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);

    // Non-blocking reads prevent pipe-buffer deadlock: if the child fills the
    // pipe buffer (64 KB on Linux) before exiting, its write() would block
    // forever while the parent is stuck in waitpid.  With O_NONBLOCK we drain
    // continuously and EAGAIN just means "no more data right now".
    if (::fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK) < 0 ||
        ::fcntl(stderrPipe[0], F_SETFL, O_NONBLOCK) < 0)
    {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        ::close(stdoutPipe[0]);
        ::close(stderrPipe[0]);
        throw std::runtime_error("fcntl O_NONBLOCK failed");
    }

    auto executor = co_await asio::this_coro::executor;
    auto start = std::chrono::steady_clock::now();
    asio::steady_timer timer(executor);
    RunResult result;
    int status = 0;

    auto drainPipe = [](int fd, std::string& out) {
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        {
            out.append(buf, static_cast<size_t>(n));
        }
        // EAGAIN/EWOULDBLOCK: no data right now — not an error
    };

    while (true)
    {
        drainPipe(stdoutPipe[0], result.stdoutStr);
        drainPipe(stderrPipe[0], result.stderrStr);

        pid_t finished = ::waitpid(pid, &status, WNOHANG);
        if (finished == pid)
        {
            break;
        }
        if (finished < 0)
        {
            ::close(stdoutPipe[0]);
            ::close(stderrPipe[0]);
            throw std::runtime_error("waitpid failed: " +
                                     std::string(::strerror(errno)));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed > config_.timeouts.installMs)
        {
            result.timedOut = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        timer.expires_after(std::chrono::milliseconds(50));
        co_await timer.async_wait(asio::use_awaitable);
    }

    // Final drain: child may have written after our last drainPipe above
    drainPipe(stdoutPipe[0], result.stdoutStr);
    drainPipe(stderrPipe[0], result.stderrStr);
    ::close(stdoutPipe[0]);
    ::close(stderrPipe[0]);

    if (result.timedOut)
    {
        result.code = 124;
    }
    else if (WIFEXITED(status))
    {
        result.code = WEXITSTATUS(status);
    }
    else
    {
        result.code = 1;
    }
    co_return result;
}
