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

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr int kDefaultInstallTimeoutMs = 30000;
constexpr int kDefaultDotCakInitTimeoutMs = 30000;
constexpr int kHmcRequestTimeoutMs = 3000;
constexpr size_t kDefaultCakMaxBytes = 16 * 1024;
constexpr const char* kDefaultPayloadFilename = "cak_payload.json";
constexpr int kDefaultCakInstallRetries = 30;
constexpr const char* kDefaultKeyStorePath = "/var/lib/dot-keyd";

// D-Bus constants for useDefaultDbus mode
constexpr const char* kNsmService = "xyz.openbmc_project.NSM";
constexpr const char* kDotActionIntf = "com.nvidia.Dot.Action";
constexpr const char* kDotStateProp = "DOTState";
constexpr const char* kCakSchemeEcdsa =
    "com.nvidia.Dot.Action.KeyAuthScheme.Ecdsa";
constexpr const char* kAsyncStatusIntf = "com.nvidia.Async.Status";
constexpr const char* kAsyncStatusProp = "Status";
constexpr const char* kAsyncStatusInProgress =
    "com.nvidia.Async.Status.AsyncOperationStatus.InProgress";
constexpr const char* kAsyncStatusSuccess =
    "com.nvidia.Async.Status.AsyncOperationStatus.Success";
constexpr const char* kBootRawService = "xyz.openbmc_project.State.Boot.Raw";
constexpr const char* kBootCakPath = "/xyz/openbmc_project/state/boot/cak0";
constexpr const char* kBootProgressIntf =
    "xyz.openbmc_project.State.Boot.Progress";
constexpr const char* kBootProgressOemProp = "BootProgressOem";
constexpr int kDbusAsyncPollIntervalMs = 500;

struct ExitCode
{
    static constexpr int kSuccess = 0;
    static constexpr int kInvalidArgs = 1;
    static constexpr int kInvalidCak = 2;
    static constexpr int kStorageError = 3;
    static constexpr int kProvisioningError = 4;
    static constexpr int kL1ResetError = 5;
};

struct HmcConfig
{
    std::string host;
    std::optional<std::string> username;
    std::optional<std::string> password;
    bool verifyTls{false};
    std::string dotCakInitPath{"/redfish/v1/Systems/HGX_Baseboard_0"};
    std::vector<std::string> installPaths{
        "/redfish/v1/Chassis/HGX_CPU_0/TrustedComponents/IRoT_CPU_0/Oem/"
        "Nvidia/DOT/Actions/NvidiaDOT.Install",
        "/redfish/v1/Chassis/HGX_CPU_1/TrustedComponents/IRoT_CPU_1/Oem/"
        "Nvidia/DOT/Actions/NvidiaDOT.Install",
    };
    std::vector<std::string> statusPaths{
        "/redfish/v1/Chassis/HGX_CPU_0/TrustedComponents/IRoT_CPU_0/Oem/"
        "Nvidia/DOT",
        "/redfish/v1/Chassis/HGX_CPU_1/TrustedComponents/IRoT_CPU_1/Oem/"
        "Nvidia/DOT",
    };
};

struct HmclessExecConfig
{
    std::string path;
    std::vector<std::string> args;
};

struct L1ResetConfig
{
    int i2cBus{70};
    std::string i2cAddr{"0x38"};
};

struct TimeoutsConfig
{
    int installMs{kDefaultInstallTimeoutMs};
    int dotCakInitMs{kDefaultDotCakInitTimeoutMs};
};

struct Config
{
    fs::path keyStorePath;
    std::optional<HmcConfig> hmc;
    std::optional<HmclessExecConfig> hmclessExec;
    bool useDefaultDbus{false};
    std::vector<std::string> dbusDotPaths;
    L1ResetConfig l1Reset;
    TimeoutsConfig timeouts;
    size_t maxCakBytes{kDefaultCakMaxBytes};
    bool allowCakReadout{true};
    std::optional<int> minimumSecurityVersion;
    int cakInstallRetries{kDefaultCakInstallRetries};
};

struct RunResult
{
    int code{0};
    std::string stdoutStr;
    std::string stderrStr;
    bool timedOut{false};
};

struct Args
{
    std::string configPath;
    bool install{false};
    bool deleteCak{false};
    std::optional<std::string> cakPath;
    std::optional<std::string> keyStorePath;
    std::optional<std::string> hmclessExec;
    std::vector<std::string> hmclessArgs;
    std::optional<int> i2cBus;
    std::optional<std::string> i2cAddr;
};
