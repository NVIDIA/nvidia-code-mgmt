/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace
{

std::filesystem::path codeManagerBinary()
{
    if (const char* binary = std::getenv("CODE_MANAGER_BINARY");
        binary != nullptr)
    {
        return binary;
    }

    char exePath[PATH_MAX] = {};
    const auto length =
        readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (length <= 0)
    {
        ADD_FAILURE() << "readlink(/proc/self/exe) failed: "
                      << std::strerror(errno);
        return {};
    }
    return std::filesystem::path(std::string(exePath, length))
               .parent_path()
               .parent_path() /
           "src" / "code-manager";
}

int runBinary(const std::vector<std::string>& args)
{
    auto binary = codeManagerBinary();
    if (!std::filesystem::exists(binary))
    {
        ADD_FAILURE() << "missing code-manager binary: " << binary;
        return 127;
    }

    std::vector<std::string> argvStorage;
    argvStorage.emplace_back(binary.string());
    argvStorage.insert(argvStorage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(argvStorage.size() + 1);
    for (auto& arg : argvStorage)
    {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const auto pid = fork();
    if (pid == -1)
    {
        ADD_FAILURE() << "fork failed: " << std::strerror(errno);
        return 127;
    }
    if (pid == 0)
    {
        const auto devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0)
        {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        execv(argv.front(), argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
    {
        ADD_FAILURE() << "waitpid failed: " << std::strerror(errno);
        return 127;
    }
    if (!WIFEXITED(status))
    {
        ADD_FAILURE() << "child terminated abnormally";
        return 127;
    }
    return WEXITSTATUS(status);
}

} // namespace

TEST(CodeManagerBinarySmokeTest, RejectsInvalidOption)
{
    EXPECT_NE(runBinary({"-z"}), 0);
}

TEST(CodeManagerBinarySmokeTest, RejectsUnsupportedUpdater)
{
    EXPECT_NE(runBinary({"-u", "Nope"}), 0);
}

TEST(CodeManagerBinarySmokeTest, SupportsDebugTokenInstallUpdater)
{
    EXPECT_NE(runBinary({"-u", "DebugTokenInstall"}), 127);
}

TEST(CodeManagerBinarySmokeTest, SupportsDebugTokenEraseUpdater)
{
    EXPECT_NE(runBinary({"-u", "DebugTokenErase"}), 127);
}

TEST(CodeManagerBinarySmokeTest, ParsesTargetAndModelArguments)
{
    EXPECT_NE(
        runBinary({"-u", "DebugTokenInstall", "-i", "GPU0", "-m", "HGX", "-n"}),
        127);
}
