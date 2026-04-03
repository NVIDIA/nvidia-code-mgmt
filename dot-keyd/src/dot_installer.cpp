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

#include "dot_installer.hpp"

#include "binary_installer.hpp"
#include "cak.hpp"
#include "dbus_installer.hpp"
#include "hmc_installer.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// DotInstaller::run  (template method skeleton)
// ---------------------------------------------------------------------------

asio::awaitable<void> DotInstaller::run()
{
    if (co_await shouldSkip())
    {
        lg2::info("CAK already installed, skipping installation");
        co_return;
    }

    lg2::info("Starting CAK provisioning");
    fs::path payloadFile = payloadPath(config_.keyStorePath);
    if (!fs::exists(payloadFile))
    {
        throw std::runtime_error("No CAK payload found");
    }

    json payload;
    try
    {
        payload = json::parse(readFile(payloadFile));
        validateDotPayload(payload);
    }
    catch (const json::parse_error& ex)
    {
        throw std::runtime_error("Invalid JSON in CAK payload file: " +
                                 std::string(ex.what()));
    }
    catch (const std::exception& ex)
    {
        throw std::runtime_error("Invalid or unreadable CAK payload file: " +
                                 std::string(ex.what()));
    }

    co_await doPreInstallCheck();
    co_await doInstall(payload);

    lg2::info("Performing L1 reset");
    l1Reset();

    co_await doVerify();
    lg2::info("CAK provisioning completed");
}

// ---------------------------------------------------------------------------
// DotInstaller::l1Reset and I2C helpers
// ---------------------------------------------------------------------------

int DotInstaller::parseI2cAddr(const std::string& addrStr)
{
    size_t parsed = 0;
    int addr = std::stoi(addrStr, &parsed, 0);
    if (parsed != addrStr.size())
    {
        throw std::runtime_error("Invalid I2C address: " + addrStr);
    }
    if (addr < 0 || addr > 0x7f)
    {
        throw std::runtime_error("I2C address out of range: " + addrStr);
    }
    return addr;
}

void DotInstaller::writeI2cBytes(int bus, int addr,
                                 const std::vector<uint8_t>& bytes)
{
    std::string devPath = "/dev/i2c-" + std::to_string(bus);
    lg2::debug("I2C write: opening {DEV}, addr=0x{ADDR}, len={LEN}", "DEV",
               devPath, "ADDR", lg2::hex, addr, "LEN", bytes.size());

    int fd = ::open(devPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        int err = errno;
        throw std::runtime_error("Failed to open " + devPath + ": " +
                                 std::strerror(err));
    }

    if (::ioctl(fd, I2C_SLAVE, addr) < 0)
    {
        int err = errno;
        ::close(fd);
        throw std::runtime_error("Failed to set I2C address " +
                                 std::to_string(addr) + " on " + devPath +
                                 ": " + std::strerror(err));
    }

    ssize_t written = ::write(fd, bytes.data(), bytes.size());
    int err = errno;
    ::close(fd);

    if (written < 0)
    {
        throw std::runtime_error("I2C write failed on " + devPath + ": " +
                                 std::strerror(err));
    }
    if (static_cast<size_t>(written) != bytes.size())
    {
        throw std::runtime_error("I2C short write on " + devPath + " (wrote " +
                                 std::to_string(written) + " of " +
                                 std::to_string(bytes.size()) + " bytes)");
    }
    lg2::debug("I2C write succeeded: {DEV} addr=0x{ADDR}, wrote={LEN} bytes",
               "DEV", devPath, "ADDR", lg2::hex, addr, "LEN", written);
}

void DotInstaller::l1Reset()
{
    lg2::info("Starting L1 reset via I2C (bus {BUS}, addr {ADDR})", "BUS",
              config_.l1Reset.i2cBus, "ADDR", config_.l1Reset.i2cAddr);

    const int addr = parseI2cAddr(config_.l1Reset.i2cAddr);
    // F0: Set Write Address to 0x00004000 (sw_main_rst)
    const std::vector<uint8_t> f0 = {0xF0, 0x04, 0x00, 0x40, 0x00, 0x00};
    // F2: Block Write — value 0x00000001 triggers the reset
    const std::vector<uint8_t> f2 = {0xF2, 0x04, 0x01, 0x00, 0x00, 0x00};
    // Flush F2: sent before each retry to drain any pending F0 state left by
    // a previous failed attempt. Writes 0x00000000 (no-op) to whatever address
    // F0 left pending. If no F0 was pending the device NACKs with Invalid
    // Sequence — non-fatal, we ignore it.
    const std::vector<uint8_t> f2Flush = {0xF2, 0x04, 0x00, 0x00, 0x00, 0x00};

    std::string lastError;
    for (int attempt = 1; attempt <= 5; ++attempt)
    {
        if (attempt > 1)
        {
            try
            {
                writeI2cBytes(config_.l1Reset.i2cBus, addr, f2Flush);
                lg2::debug("L1 reset flush F2 sent before retry {ATTEMPT}",
                           "ATTEMPT", attempt);
            }
            catch (const std::exception& ex)
            {
                // Expected if no F0 was pending (Invalid Sequence NACK).
                lg2::debug("L1 reset flush F2 ignored: {ERROR}", "ERROR",
                           ex.what());
            }
        }

        try
        {
            writeI2cBytes(config_.l1Reset.i2cBus, addr, f0);
            writeI2cBytes(config_.l1Reset.i2cBus, addr, f2);
            lg2::info("L1 reset completed successfully on attempt {ATTEMPT}",
                      "ATTEMPT", attempt);
            return;
        }
        catch (const std::exception& ex)
        {
            lastError = ex.what();
        }
        lg2::warning("L1 reset attempt {ATTEMPT}/5 failed: {ERROR}", "ATTEMPT",
                     attempt, "ERROR", lastError);
        if (attempt < 5)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }
    throw std::runtime_error("L1 reset failed after 5 attempts: " + lastError);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<DotInstaller>
    createInstaller(const Config& config,
                    std::shared_ptr<sdbusplus::asio::connection> bus)
{
    if (config.useDefaultDbus)
    {
        return std::make_unique<DbusInstaller>(config, bus);
    }
    if (config.hmc)
    {
        return std::make_unique<HmcInstaller>(config, bus);
    }
    return std::make_unique<BinaryInstaller>(config, bus);
}
