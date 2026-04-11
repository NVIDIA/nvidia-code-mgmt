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

#include "config.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace asio = boost::asio;

/**
 * Thrown when CAK installation is permanently declined by the endpoint
 * (e.g. HTTP 409 — already installed, or policy rejection).
 * retryInstall() propagates this immediately without retrying.
 */
class CakInstallDeclinedException : public std::runtime_error
{
  public:
    explicit CakInstallDeclinedException(const std::string& msg) :
        std::runtime_error(msg)
    {}
};

/**
 * Base class for CAK installers.  Implements the Template Method pattern:
 * run() defines the invariant skeleton (load → install → L1 reset → verify)
 * while subclasses supply the flow-specific steps via small virtual methods
 * that delegate to private helpers.
 */
class DotInstaller
{
  public:
    virtual ~DotInstaller() = default;

    asio::awaitable<void> run();

  protected:
    explicit DotInstaller(const Config& config,
                          std::shared_ptr<sdbusplus::asio::connection> bus) :
        config_(config), bus_(std::move(bus))
    {}

    Config config_;
    std::shared_ptr<sdbusplus::asio::connection> bus_;

    // Override points — keep these small; do the work in private methods.
    virtual asio::awaitable<bool> shouldSkip()
    {
        co_return false;
    }
    virtual asio::awaitable<void> doPreInstallCheck()
    {
        co_return;
    }
    virtual asio::awaitable<void> doInstall(const json& payload) = 0;
    virtual asio::awaitable<void> doVerify()
    {
        co_return;
    }
    virtual asio::awaitable<void> doL1Reset();

    // Retry loop for install operations.  fn(attempt) must return
    // awaitable<void>.
    template <typename Fn>
    asio::awaitable<void> retryInstall(const std::string& label, Fn fn)
    {
        std::string lastError;
        lg2::info("Starting {LABEL} CAK installation (max {RETRIES} attempts)",
                  "LABEL", label, "RETRIES", config_.cakInstallRetries);

        for (int attempt = 1; attempt <= config_.cakInstallRetries; ++attempt)
        {
            try
            {
                lg2::info("{LABEL} CAK installation attempt {ATTEMPT}/{MAX}",
                          "LABEL", label, "ATTEMPT", attempt, "MAX",
                          config_.cakInstallRetries);
                co_await fn(attempt);
                lg2::info(
                    "{LABEL} CAK installation succeeded on attempt {ATTEMPT}",
                    "LABEL", label, "ATTEMPT", attempt);
                co_return;
            }
            catch (const CakInstallDeclinedException&)
            {
                throw; // permanent decline — do not retry
            }
            catch (const boost::system::system_error& ex)
            {
                if (ex.code() == asio::error::operation_aborted)
                    throw;
                lastError = ex.what();
            }
            catch (const std::exception& ex)
            {
                lastError = ex.what();
            }
            lg2::warning("{LABEL} attempt {ATTEMPT}/{MAX} failed: {ERROR}",
                         "LABEL", label, "ATTEMPT", attempt, "MAX",
                         config_.cakInstallRetries, "ERROR", lastError);
            if (attempt < config_.cakInstallRetries)
            {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
        }
        throw std::runtime_error(std::string(label) +
                                 " CAK installation failed after " +
                                 std::to_string(config_.cakInstallRetries) +
                                 " attempts: " + lastError);
    }
};

// Factory — selects the right subclass based on config.
std::unique_ptr<DotInstaller>
    createInstaller(const Config& config,
                    std::shared_ptr<sdbusplus::asio::connection> bus);
