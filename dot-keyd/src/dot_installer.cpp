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

#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

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
    co_await doL1Reset();

    co_await doVerify();
    lg2::info("CAK provisioning completed");
}

// ---------------------------------------------------------------------------
// DotInstaller::doL1Reset (default — no-op)
// ---------------------------------------------------------------------------

/** Default L1 reset: no-op.  HmcInstaller overrides this to perform the
 *  reset via SSH + USB control transfer to the CPU.  DbusInstaller and
 *  BinaryInstaller will add USB-direct reset support once the HMC path is
 *  confirmed working. */
asio::awaitable<void> DotInstaller::doL1Reset()
{
    lg2::warning("L1 reset not implemented for this installer, skipping");
    co_return;
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
