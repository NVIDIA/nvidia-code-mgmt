/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

// Coverage for the standalone usbrcm-recovery-tool entry point: the JSON pin
// config loader (loadPinConfig) and the CLI subcommand dispatch in main().
// The recovery primitives are stubbed so no USB or GPIO access is attempted.

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#define main usbrcm_tool_main
#include "../recovery_tool/usbrcm_recovery_tool/main.cpp"
#undef main

namespace
{

// --- stubs for the recovery primitives main.cpp dispatches to ---

nlohmann::json forceResult = {{"Status", "Successful"}};
int forceCallCount = 0;
RecoveryPinConfig lastForceCfg;

bool statusResult = true;
int statusCallCount = 0;
bool statusVerbose = false;

bool performResult = true;
int performCallCount = 0;
std::string performPort;
std::vector<std::string> performImages;
std::string performBlob;
bool performVerbose = false;

class UsbRcmMainTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        forceResult = {{"Status", "Successful"}};
        forceCallCount = 0;
        lastForceCfg = {};
        statusResult = true;
        statusCallCount = 0;
        statusVerbose = false;
        performResult = true;
        performCallCount = 0;
        performPort.clear();
        performImages.clear();
        performBlob.clear();
        performVerbose = false;

        dir = std::filesystem::temp_directory_path() /
              ("usbrcm_main_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string writeConfig(const nlohmann::json& j,
                            const std::string& name = "cfg.json")
    {
        auto path = dir / name;
        std::ofstream(path) << j.dump();
        return path.string();
    }

    static nlohmann::json gpioResetConfig()
    {
        return {
            {"ResetPins", {"RST_A"}},
            {"StrapPins", {"STRAP_A", "STRAP_B"}},
            {"StrapActiveValues", {0, 1}},
            {"StrapDefaultValues", {1, 0}},
        };
    }

    static nlohmann::json dbusResetConfig()
    {
        return {
            {"ResetDbusService", "com.example.Service"},
            {"ResetDbusObject", "/com/example/obj"},
            {"ResetDbusInterface", "com.example.Iface"},
            {"ResetDbusMethod", "Reset"},
            {"StrapPins", {"STRAP_A"}},
            {"StrapActiveValues", {0}},
            {"StrapDefaultValues", {1}},
        };
    }

    static int runMain(std::vector<std::string> args)
    {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("usbrcm-recovery-tool"));
        for (auto& a : args)
        {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        return usbrcm_tool_main(static_cast<int>(argv.size()) - 1, argv.data());
    }

    std::filesystem::path dir;
};

} // namespace

void forceRecoveryMode(const RecoveryPinConfig& cfg, nlohmann::json& out)
{
    ++forceCallCount;
    lastForceCfg = cfg;
    out = forceResult;
}

void setGPIODefaultPinStates(const RecoveryPinConfig& /*cfg*/,
                             nlohmann::json& out)
{
    out = {{"Status", "Successful"}};
}

bool getRecoveryStatus(nlohmann::json& jsonOutput, bool verbose)
{
    ++statusCallCount;
    statusVerbose = verbose;
    jsonOutput = {{"Status", statusResult ? "Successful" : "Failed"}};
    return statusResult;
}

bool performUsbRecovery(const std::string& portPath,
                        const std::vector<std::string>& imagePaths,
                        const std::string& blobPath, nlohmann::json& jsonOutput,
                        bool verbose)
{
    ++performCallCount;
    performPort = portPath;
    performImages = imagePaths;
    performBlob = blobPath;
    performVerbose = verbose;
    jsonOutput = {{"Status", performResult ? "Successful" : "Failed"}};
    return performResult;
}

// --- loadPinConfig ---

TEST_F(UsbRcmMainTest, LoadPinConfigParsesGpioResetMode)
{
    auto cfg = loadPinConfig(writeConfig(gpioResetConfig()));
    EXPECT_EQ(cfg.resetPins, (std::vector<std::string>{"RST_A"}));
    EXPECT_TRUE(cfg.dbusService.empty());
    EXPECT_EQ(cfg.strapPins, (std::vector<std::string>{"STRAP_A", "STRAP_B"}));
    EXPECT_EQ(cfg.strapActiveValues, (std::vector<int>{0, 1}));
    EXPECT_EQ(cfg.strapDefaultValues, (std::vector<int>{1, 0}));
}

TEST_F(UsbRcmMainTest, LoadPinConfigParsesDbusResetMode)
{
    auto cfg = loadPinConfig(writeConfig(dbusResetConfig()));
    EXPECT_TRUE(cfg.resetPins.empty());
    EXPECT_EQ(cfg.dbusService, "com.example.Service");
    EXPECT_EQ(cfg.dbusObject, "/com/example/obj");
    EXPECT_EQ(cfg.dbusInterface, "com.example.Iface");
    EXPECT_EQ(cfg.dbusMethod, "Reset");
    EXPECT_EQ(cfg.strapPins, (std::vector<std::string>{"STRAP_A"}));
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsUnreadableFile)
{
    EXPECT_THROW(loadPinConfig((dir / "does-not-exist.json").string()),
                 std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsMalformedJson)
{
    auto path = dir / "bad.json";
    std::ofstream(path) << "{ not json";
    EXPECT_THROW(loadPinConfig(path.string()), nlohmann::json::exception);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsMissingRequiredKeys)
{
    auto j = gpioResetConfig();
    j.erase("StrapPins");
    EXPECT_THROW(loadPinConfig(writeConfig(j)), nlohmann::json::exception);

    // Neither ResetPins nor the D-Bus destination: falls into the D-Bus
    // branch and fails on the first missing key.
    auto k = gpioResetConfig();
    k.erase("ResetPins");
    EXPECT_THROW(loadPinConfig(writeConfig(k)), nlohmann::json::exception);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsEmptyStrapPins)
{
    auto j = gpioResetConfig();
    j["StrapPins"] = nlohmann::json::array();
    j["StrapActiveValues"] = nlohmann::json::array();
    j["StrapDefaultValues"] = nlohmann::json::array();
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsActiveValuesLengthMismatch)
{
    auto j = gpioResetConfig();
    j["StrapActiveValues"] = {0};
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsDefaultValuesLengthMismatch)
{
    auto j = gpioResetConfig();
    j["StrapDefaultValues"] = {1};
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsNonBinaryActiveValue)
{
    auto j = gpioResetConfig();
    j["StrapActiveValues"] = {0, 2};
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsNonBinaryDefaultValue)
{
    auto j = gpioResetConfig();
    j["StrapDefaultValues"] = {-1, 0};
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsEmptyResetPinsArray)
{
    // ResetPins present but empty, with no D-Bus fallback configured.
    auto j = gpioResetConfig();
    j["ResetPins"] = nlohmann::json::array();
    EXPECT_THROW(loadPinConfig(writeConfig(j)), std::runtime_error);
}

TEST_F(UsbRcmMainTest, LoadPinConfigRejectsEachBlankDbusField)
{
    // One sub-case per field so every short-circuit term of the D-Bus
    // destination check is exercised.
    for (const auto* field : {"ResetDbusService", "ResetDbusObject",
                              "ResetDbusInterface", "ResetDbusMethod"})
    {
        auto j = dbusResetConfig();
        j[field] = "";
        EXPECT_THROW(
            loadPinConfig(writeConfig(j, std::string(field) + ".json")),
            std::runtime_error)
            << "blank " << field << " should be rejected";
    }
}

// --- main() dispatch ---

TEST_F(UsbRcmMainTest, MainSetForceRecoverySucceeds)
{
    auto path = writeConfig(gpioResetConfig());
    EXPECT_EQ(runMain({"SetForceRecovery", "-c", path}), EXIT_SUCCESS);
    EXPECT_EQ(forceCallCount, 1);
    EXPECT_EQ(lastForceCfg.resetPins, (std::vector<std::string>{"RST_A"}));
    EXPECT_EQ(lastForceCfg.strapPins.size(), 2u);
}

TEST_F(UsbRcmMainTest, MainSetForceRecoveryReportsFailedStatus)
{
    forceResult = {{"Status", "Failed"}, {"Error", "boom"}};
    auto path = writeConfig(gpioResetConfig());
    EXPECT_EQ(runMain({"SetForceRecovery", "--config-file", path}),
              EXIT_FAILURE);
    EXPECT_EQ(forceCallCount, 1);
}

TEST_F(UsbRcmMainTest, MainSetForceRecoveryToleratesMissingStatus)
{
    forceResult = nlohmann::json::object();
    auto path = writeConfig(gpioResetConfig());
    EXPECT_EQ(runMain({"SetForceRecovery", "-c", path}), EXIT_SUCCESS);
}

TEST_F(UsbRcmMainTest, MainSetForceRecoveryFailsOnBadConfigFile)
{
    EXPECT_EQ(
        runMain({"SetForceRecovery", "-c", (dir / "missing.json").string()}),
        EXIT_FAILURE);
    EXPECT_EQ(forceCallCount, 0);
}

TEST_F(UsbRcmMainTest, MainSetForceRecoveryRequiresConfigOption)
{
    EXPECT_NE(runMain({"SetForceRecovery"}), EXIT_SUCCESS);
    EXPECT_EQ(forceCallCount, 0);
}

TEST_F(UsbRcmMainTest, MainGetRecoveryStatusCoversSuccessAndFailure)
{
    EXPECT_EQ(runMain({"GetRecoveryStatus", "-v"}), EXIT_SUCCESS);
    EXPECT_EQ(statusCallCount, 1);
    EXPECT_TRUE(statusVerbose);

    statusResult = false;
    EXPECT_EQ(runMain({"GetRecoveryStatus"}), EXIT_FAILURE);
    EXPECT_EQ(statusCallCount, 2);
    EXPECT_FALSE(statusVerbose);
}

TEST_F(UsbRcmMainTest, MainPerformUsbRecoveryCoversSuccessAndFailure)
{
    EXPECT_EQ(runMain({"PerformUSBRecovery", "-p", "1-1.3", "-i", "a.bin",
                       "b.bin", "-b", "dot.blob", "-v"}),
              EXIT_SUCCESS);
    EXPECT_EQ(performCallCount, 1);
    EXPECT_EQ(performPort, "1-1.3");
    EXPECT_EQ(performImages, (std::vector<std::string>{"a.bin", "b.bin"}));
    EXPECT_EQ(performBlob, "dot.blob");
    EXPECT_TRUE(performVerbose);

    performResult = false;
    EXPECT_EQ(runMain({"PerformUSBRecovery", "-p", "2-4", "-i", "x.bin"}),
              EXIT_FAILURE);
    EXPECT_EQ(performCallCount, 2);
    EXPECT_TRUE(performBlob.empty());
    EXPECT_FALSE(performVerbose);
}

TEST_F(UsbRcmMainTest, MainRejectsMissingSubcommand)
{
    EXPECT_NE(runMain({}), EXIT_SUCCESS);
    EXPECT_EQ(forceCallCount + statusCallCount + performCallCount, 0);
}
