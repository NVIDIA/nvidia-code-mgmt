/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gtest/gtest.h"
#undef FAIL

#include "../debug_token/token_utility.hpp"

#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

constexpr std::array<uint8_t, 8> matchingSerial = {0x01, 0x1E, 0x02, 0x0E,
                                                   0x16, 0x0A, 0x10, 0x17};

std::filesystem::path makeTempArtifactPath(const std::string& prefix,
                                           const std::string& suffix)
{
    return std::filesystem::temp_directory_path() /
           (prefix + "-" + std::to_string(getpid()) + "-" +
            std::to_string(::random()) + suffix);
}

std::filesystem::path preloadLibrary()
{
    if (const char* preload = std::getenv("DEBUG_TOKEN_PRELOAD_LIBRARY");
        preload != nullptr)
    {
        return preload;
    }

    return {};
}

std::filesystem::path debugTokenBinary()
{
    if (const char* binary = std::getenv("DEBUG_TOKEN_BINARY");
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
           "debug_token" / "updateDebugToken";
}

std::filesystem::path makeLegacyTokenImage(
    const std::array<uint8_t, 8>& serialNumber = matchingSerial)
{
    auto tmpPath = makeTempArtifactPath("debug-token-bin", ".bin");

    std::ofstream file(tmpPath, std::ios::binary);
    if (!file.is_open())
    {
        ADD_FAILURE() << "failed to open token image: " << tmpPath;
        return {};
    }

    TokenHeader tokenHdr{};
    std::memcpy(tokenHdr.identifier, "EDTI", 4);
    tokenHdr.versionMinor = 0;
    tokenHdr.versionMajor = 1;
    tokenHdr.structSize = 256;
    tokenHdr.tokenType = 1;

    std::vector<uint8_t> tokenData(256, 0);
    std::memcpy(tokenData.data(), &tokenHdr, sizeof(TokenHeader));
    std::memcpy(tokenData.data() + sizeof(TokenHeader), serialNumber.data(),
                serialNumber.size());

    DebugTokenHeader fileHdr{};
    fileHdr.version = 1;
    fileHdr.type = 2;
    fileHdr.numberOfRecords = 1;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
    fileHdr.fileSize =
        static_cast<uint32_t>(sizeof(DebugTokenHeader) + tokenData.size());

    file.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
    file.write(reinterpret_cast<const char*>(tokenData.data()),
               tokenData.size());
    file.close();

    EXPECT_TRUE(std::filesystem::exists(tmpPath));
    return tmpPath;
}

int runBinary(const std::vector<std::string>& args,
              const std::string& scenario = {})
{
    auto binary = debugTokenBinary();
    if (!std::filesystem::exists(binary))
    {
        ADD_FAILURE() << "missing updateDebugToken binary: " << binary;
        return 127;
    }
    const auto markerPath =
        scenario.empty() ? std::filesystem::path{}
                         : makeTempArtifactPath("debug-token-preload", ".log");

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
        if (!scenario.empty())
        {
            auto preload = preloadLibrary();
            if (preload.empty())
            {
                _exit(126);
            }
            setenv("LD_PRELOAD", preload.c_str(), 1);
            setenv("DEBUG_TOKEN_PRELOAD_SCENARIO", scenario.c_str(), 1);
            setenv("DEBUG_TOKEN_PRELOAD_MARKER", markerPath.c_str(), 1);
        }

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

    if (!scenario.empty())
    {
        EXPECT_TRUE(std::filesystem::exists(markerPath));
        std::ifstream marker(markerPath);
        std::string contents((std::istreambuf_iterator<char>(marker)),
                             std::istreambuf_iterator<char>());
        EXPECT_NE(contents.find("init"), std::string::npos);
        if (scenario == "direct_probe_public_methods")
        {
            EXPECT_NE(contents.find("probe_complete"), std::string::npos);
        }
        else
        {
            EXPECT_NE(contents.find("sd_bus_call"), std::string::npos);
        }
        std::filesystem::remove(markerPath);
    }

    return WEXITSTATUS(status);
}

} // namespace

TEST(DebugTokenBinarySmokeTest, RejectsMissingArguments)
{
    EXPECT_EQ(runBinary({}), 255);
}

TEST(DebugTokenBinarySmokeTest, RejectsNonNumericOperation)
{
    EXPECT_EQ(runBinary({"abc", "1.0"}), 255);
}

TEST(DebugTokenBinarySmokeTest, AcceptsUnknownOperation)
{
    EXPECT_EQ(runBinary({"99", "1.0"}), 0);
}

TEST(DebugTokenBinarySmokeTest, ExercisesErasePath)
{
    EXPECT_EQ(runBinary({"0", "1.0"}), 0);
}

TEST(DebugTokenBinarySmokeTest, RejectsInstallWithoutTokenPath)
{
    EXPECT_EQ(runBinary({"1", "1.0"}), 255);
}

TEST(DebugTokenBinarySmokeTest, ExercisesInstallFailurePath)
{
    EXPECT_EQ(runBinary({"1", "1.0", "/tmp/does-not-exist.bin"}), 0);
}

TEST(DebugTokenBinarySmokeTest, ExercisesInstallWithSingleTokenImage)
{
    EXPECT_EQ(runBinary({"1", "1.0", "debug_token_single.bin"}), 0);
}

TEST(DebugTokenBinarySmokeTest, ExercisesInstallWithMultipleTokenImage)
{
    EXPECT_EQ(runBinary({"1", "1.0", "debug_token_multiple.bin"}), 0);
}

TEST(DebugTokenBinarySmokeTest, EraseAutomaticMixedOutcomes)
{
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_auto_mixed"), 0);
}

TEST(DebugTokenBinarySmokeTest, EraseAutomaticSignalSuccess)
{
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_auto_signal_success"), 0);
}

TEST(DebugTokenBinarySmokeTest, EraseAutomaticSignalFailure)
{
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_auto_signal_failure"), 0);
}

TEST(DebugTokenBinarySmokeTest, EraseAutomaticNoEndpoints)
{
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_auto_no_endpoints"), 0);
}

TEST(DebugTokenBinarySmokeTest, ErasePolicyFailureScenarios)
{
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_policy_empty_then_enum_fail"), 0);
    EXPECT_EQ(runBinary({"0", "1.0"}, "erase_policy_multi_then_enum_fail"), 0);
    EXPECT_EQ(
        runBinary({"0", "1.0"}, "erase_policy_property_fail_then_enum_fail"),
        0);
}

TEST(DebugTokenBinarySmokeTest, InstallMixedOutcomes)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(runBinary({"1", "1.0", tokenPath.string()}, "install_mixed"), 0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallSignalSuccess)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(
        runBinary({"1", "1.0", tokenPath.string()}, "install_signal_success"),
        0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallSignalFailure)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(
        runBinary({"1", "1.0", tokenPath.string()}, "install_signal_failure"),
        0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallNoEndpoints)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(
        runBinary({"1", "1.0", tokenPath.string()}, "install_no_endpoints"), 0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallAllMissingTokens)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(runBinary({"1", "1.0", tokenPath.string()},
                        "install_all_missing_tokens"),
              0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallPropertyWrongType)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(runBinary({"1", "1.0", tokenPath.string()},
                        "install_property_wrong_type"),
              0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, InstallEnumerateFailure)
{
    const auto tokenPath = makeLegacyTokenImage();
    EXPECT_EQ(
        runBinary({"1", "1.0", tokenPath.string()}, "install_enumerate_fail"),
        0);
    std::filesystem::remove(tokenPath);
}

TEST(DebugTokenBinarySmokeTest, DirectProbePublicMethods)
{
    EXPECT_EQ(runBinary({"99", "1.0"}, "direct_probe_public_methods"), 0);
}
