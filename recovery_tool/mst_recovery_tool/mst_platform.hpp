// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#pragma once

#include "mst_policy.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mst
{

struct DevInfo
{
    bool exists = false;
    bool isChar = false;
    bool readable = false;
    bool writable = false;
};

enum class OutputMode
{
    // stdout and stderr merged into the callback
    Merged,
    // child inherits stdout and stderr
    Inherit
};

// Everything the recovery flow does to the outside world. The production
// implementation is in mst_platform_real.cpp; tests and the diff harness
// substitute their own.
class Platform
{
  public:
    virtual ~Platform() = default;

    virtual std::expected<Value, std::string>
        getProperty(const std::string& service, const std::string& path,
                    const std::string& iface, const std::string& prop) = 0;
    virtual std::expected<PropertyMap, std::string>
        getAll(const std::string& service, const std::string& path,
               const std::string& iface) = 0;
    virtual std::expected<std::vector<std::string>, std::string>
        getSubTreePaths(const std::string& root, int32_t depth,
                        const std::vector<std::string>& ifaces) = 0;
    virtual void createLog(const std::string& message, const std::string& level,
                           const std::map<std::string, std::string>& data) = 0;

    virtual DevInfo statDevice(const std::string& path) = 0;
    virtual bool isReadableFile(const std::string& path, bool regularOnly) = 0;
    virtual std::optional<uint64_t> fileSize(const std::string& path) = 0;

    virtual std::optional<std::array<uint8_t, 4>>
        i2cWriteRead(const std::string& bus, uint16_t addr,
                     const std::vector<uint8_t>& write, bool force) = 0;

    virtual bool gpioFind(const std::string& name) = 0;
    virtual bool gpioSet(const std::string& name, int value) = 0;
    virtual bool gpioHold(const std::string& name, int value) = 0;
    virtual void gpioRelease(const std::string& name) = 0;

    // Reads the first line of a USB sysfs attribute (idVendor/idProduct) of
    // the device enumerated at `port`; nullopt when absent or unterminated.
    virtual std::optional<std::string>
        usbAttribute(const std::string& port, const std::string& attr) = 0;

    virtual bool commandExists(const std::string& name) = 0;
    virtual int
        runProcess(const std::vector<std::string>& argv, OutputMode mode,
                   const std::function<void(std::string_view)>& out) = 0;

    virtual void sleepMs(uint64_t ms) = 0;
    virtual int64_t nowSeconds() = 0;

    virtual void writeOut(std::string_view line) = 0;
    virtual void writeErr(std::string_view line) = 0;
};

} // namespace mst
