// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#pragma once

#include "mst_platform.hpp"

#include <gpiod.hpp>
#include <sdbusplus/bus.hpp>

#include <memory>
#include <mutex>

namespace mst
{

class RealPlatform : public Platform
{
  public:
    RealPlatform();

    std::expected<Value, std::string>
        getProperty(const std::string& service, const std::string& path,
                    const std::string& iface, const std::string& prop) override;
    std::expected<PropertyMap, std::string>
        getAll(const std::string& service, const std::string& path,
               const std::string& iface) override;
    std::expected<std::vector<std::string>, std::string>
        getSubTreePaths(const std::string& root, int32_t depth,
                        const std::vector<std::string>& ifaces) override;
    void createLog(const std::string& message, const std::string& level,
                   const std::map<std::string, std::string>& data) override;

    DevInfo statDevice(const std::string& path) override;
    bool isReadableFile(const std::string& path, bool regularOnly) override;
    std::optional<uint64_t> fileSize(const std::string& path) override;

    std::optional<std::array<uint8_t, 4>>
        i2cWriteRead(const std::string& bus, uint16_t addr,
                     const std::vector<uint8_t>& write, bool force) override;

    bool gpioFind(const std::string& name) override;
    bool gpioSet(const std::string& name, int value) override;
    bool gpioHold(const std::string& name, int value) override;
    void gpioRelease(const std::string& name) override;

    std::optional<std::string> usbAttribute(const std::string& port,
                                            const std::string& attr) override;

    bool commandExists(const std::string& name) override;
    int runProcess(const std::vector<std::string>& argv, OutputMode mode,
                   const std::function<void(std::string_view)>& out) override;

    void sleepMs(uint64_t ms) override;
    int64_t nowSeconds() override;

    void writeOut(std::string_view line) override;
    void writeErr(std::string_view line) override;

  protected:
    // First line of a sysfs-style attribute file, `read -r` semantics.
    static std::optional<std::string> readLine(const std::string& path);

  private:
    std::mutex busMutex;
    std::mutex gpioMutex;
    std::mutex outMutex;
    sdbusplus::bus_t bus;
    std::map<std::string, gpiod::line> held;
};

std::unique_ptr<Platform> makePlatform();

} // namespace mst
