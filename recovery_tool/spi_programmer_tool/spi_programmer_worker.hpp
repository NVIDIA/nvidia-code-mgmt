// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spi_programmer
{

inline constexpr auto defaultLayoutDir =
    "/var/emmc/user-logs/logging/spi_dumps";
// flashrom-wrapper passes this exact path to flashrom -l
inline constexpr auto layoutFileName = "sbios_boot_flash.layout";
// SBIOS SPI flash size; the layout covers it
inline constexpr std::int64_t flashSize = 0x08000000;

class Bus
{
  public:
    virtual ~Bus() = default;

    /** Progress object path returned by flashrom-wrapper, or nullopt. */
    virtual std::optional<std::string> startUpdate(const std::string& device,
                                                   int imageFd) = 0;

    /** Common.Progress Status, or nullopt when the read fails. */
    virtual std::optional<std::string>
        getStatus(const std::string& progressPath) = 0;

    /** Common.Progress Progress; the value is not used. */
    virtual void getProgress(const std::string& progressPath) = 0;

    /** Logging.Create; failures are logged, not returned. */
    virtual void createLog(const std::string& message,
                           const std::string& severity,
                           const std::map<std::string, std::string>& data) = 0;
};

class Console
{
  public:
    virtual ~Console() = default;
    virtual void out(std::string_view line) = 0;
    virtual void err(std::string_view line) = 0;
};

struct Options
{
    std::string programName = "spi-programmer";
    std::filesystem::path layoutDir = defaultLayoutDir;
    std::chrono::seconds pollInterval{5};
    std::function<void(std::chrono::seconds)> sleep;
};

std::string formatLayout(std::int64_t imageSize);

class Worker
{
  public:
    Worker(Bus& bus, Console& console, Options options);
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    /** args excludes argv[0]; returns the process exit code. */
    int run(const std::vector<std::string>& args);

    /** Exit-trap cleanup for a terminating signal; async-signal-safe. */
    void cleanupFromSignal(int outFd) noexcept;

  private:
    int execute(const std::vector<std::string>& args);
    bool generateLayout(const std::string& filePath, std::int64_t size);
    std::int64_t openImage(const std::string& filePath);
    int poll(const std::vector<std::pair<std::string, std::string>>& objects,
             const std::string& version);
    void cleanup();

    Bus& bus;
    Console& console;
    Options options;
    std::string layoutPath;
    std::string cleanupMessage;
    std::string closeMessage;
    std::atomic<int> imageFd{-1};
    std::atomic<bool> imageFdClosed{false};
};

} // namespace spi_programmer
