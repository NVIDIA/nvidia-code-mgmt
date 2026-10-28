// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#pragma once

#include "mst_platform.hpp"
#include "mst_policy.hpp"

#include <atomic>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mst
{

inline constexpr auto emService = "xyz.openbmc_project.EntityManager";
inline constexpr auto inventoryRoot = "/xyz/openbmc_project/inventory";

// Finds the MSTRecoveryPolicy record whose DeviceType matches.
std::expected<Policy, std::string> findPolicy(Platform& platform,
                                              const std::string& deviceType,
                                              unsigned attempts = 10,
                                              uint64_t retryDelayMs = 1000);

class Recovery
{
  public:
    // `program` is the basename used in usage text (argv[0] of the unit).
    Recovery(Platform& platform, Policy policy, std::string program);

    int run(std::vector<std::string> args);

    // Signal hook for FNPMode HoldDeasserted: releases the FNP holder and
    // re-asserts FNP when the recovery has not succeeded.
    static void installSignalHandlers();

  private:
    struct Device
    {
        std::string path;
        std::string name;
        std::map<std::string, std::string> props;
        std::string smaName;
        std::string smaUsbPort;
    };

    int runGroups(std::vector<std::string> args);
    bool recoverDevice(const std::string& relPath, const std::string& image,
                       bool force) noexcept;
    unsigned attempts() const;
    std::vector<std::string> deviceProperties() const;
    int runHold(std::vector<std::string> args);

    bool performMst(const std::string& relPath, const std::string& image,
                    bool force);
    bool performLegacy(const std::string& relPath, const std::string& image,
                       bool force);

    std::optional<Device> readDevice(const std::string& relPath);
    std::optional<uint32_t> readGis(const std::string& bus,
                                    const std::string& addr);
    bool reset(const Device& dev, int fnpValue);
    bool gpioFindRetry(const std::string& name);
    bool gpioSetRetry(const std::string& name, int value);
    int burn(const Device& dev, const std::string& image,
             const std::string& addrText);
    bool handleMissingBus(const Device& dev, const std::string& vidPid);
    std::optional<std::string> usbVidPid(const std::string& port);
    std::optional<std::string> mctpConnectivity(const std::string& eid);
    void recoveryEvent(const std::string& msgId, const std::string& args,
                       const std::string& severity = "Informational",
                       const std::string& resolution = {});
    void resourceError(const std::string& obj, const std::string& message,
                       const std::string& resolution);

    void info(const std::string& obj, const std::string& msg);
    void error(const std::string& obj, const std::string& msg);

    // FNPMode HoldDeasserted helpers
    [[noreturn]] void die(int rc, const std::string& msg);
    [[noreturn]] void finish(int rc);
    void tagged(const char* tag, const std::string& msg);
    void boundary();
    void cleanup();
    void logFirmwareUpdate(const std::string& level, const std::string& message,
                           const std::string& msgId, const std::string& args,
                           bool withNamespace);

    Platform& platform;
    Policy policy;
    std::string program;

    std::string deviceName;
    std::string imageVersion;
    std::string fnpGpio;
    int fnpPolarity = 0;
    std::string assertedReset;
    bool holding = false;
    bool succeeded = false;
    bool cleanedUp = false;
};

} // namespace mst
