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

#include "dbus_installer.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <sdbusplus/message/types.hpp>

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

// Wraps sdbusplus::asio::getProperty as a coroutine.
// getProperty takes std::function (requires copy-constructible), so wrap the
// move-only awaitable_handler in a shared_ptr to satisfy that requirement.
template <typename T>
asio::awaitable<T> getPropertyAwaitable(sdbusplus::asio::connection& bus,
                                        const std::string& service,
                                        const std::string& path,
                                        const std::string& interface,
                                        const std::string& property)
{
    return asio::async_initiate<const asio::use_awaitable_t<>&,
                                void(boost::system::error_code, T)>(
        [&bus, service, path, interface, property](auto handler) {
            auto sh = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));
            sdbusplus::asio::getProperty<T>(
                bus, service, path, interface, property,
                [sh](boost::system::error_code ec, T value) {
                    (*sh)(ec, std::move(value));
                });
        },
        asio::use_awaitable);
}

} // namespace

// ---------------------------------------------------------------------------
// Virtual override methods — small, delegate to private helpers
// ---------------------------------------------------------------------------

asio::awaitable<void> DbusInstaller::doInstall(const json& payload)
{
    std::set<std::string> donePaths;
    co_await retryInstall("D-Bus", [&](int) -> asio::awaitable<void> {
        co_await install(payload, donePaths);
    });
}

asio::awaitable<void> DbusInstaller::doVerify()
{
    lg2::info("Verifying DOT state after L1 reset via D-Bus");
    co_await waitForCakComplete();
    lg2::info("DOT state verification succeeded");
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

asio::awaitable<void> DbusInstaller::install(const json& payload,
                                             std::set<std::string>& donePaths)
{
    std::string cakAuthEnum = toDbusAuthScheme(
        payload["CAKKey"]["AuthenticationScheme"].get<std::string>());
    std::string cakEcdsaKey = payload["CAKKey"]["ECDSAKey"].get<std::string>();
    std::string cakLmsKey;
    std::string lakAuthEnum = kCakSchemeEcdsa;
    std::string lakEcdsaKey;
    std::string lakLmsKey;

    if (payload.contains("LAKKey") && payload["LAKKey"].is_object())
    {
        lakAuthEnum = toDbusAuthScheme(
            payload["LAKKey"]["AuthenticationScheme"].get<std::string>());
        lakEcdsaKey = payload["LAKKey"]["ECDSAKey"].get<std::string>();
    }

    bool lockDisable = payload.value("LockDisable", false);
    int rawOwnerSvn = payload.value("OwnerMinimumSecurityVersion", 0);
    int rawVendorSvn = payload.value("VendorMinimumSecurityVersion", 0);
    if (rawOwnerSvn < 0 || rawOwnerSvn > 255)
    {
        throw std::runtime_error("OwnerMinimumSecurityVersion out of range: " +
                                 std::to_string(rawOwnerSvn));
    }
    if (rawVendorSvn < 0 || rawVendorSvn > 255)
    {
        throw std::runtime_error("VendorMinimumSecurityVersion out of range: " +
                                 std::to_string(rawVendorSvn));
    }
    auto ownerMinSvn = static_cast<uint8_t>(rawOwnerSvn);
    auto vendorMinSvn = static_cast<uint8_t>(rawVendorSvn);

    if (config_.dbusDotPaths.empty())
    {
        throw std::runtime_error("dbusDotPaths not configured");
    }

    for (const auto& objPath : config_.dbusDotPaths)
    {
        if (donePaths.count(objPath))
        {
            lg2::info("Skipping already installed path {PATH}", "PATH",
                      objPath);
            continue;
        }

        lg2::info("Calling DotCAKInstall on {PATH}", "PATH", objPath);

        auto asyncObjPath = co_await asio::async_initiate<
            const asio::use_awaitable_t<>&,
            void(boost::system::error_code, sdbusplus::message::object_path)>(
            [this, &objPath, &cakAuthEnum, &cakEcdsaKey, &cakLmsKey,
             &lakAuthEnum, &lakEcdsaKey, &lakLmsKey, lockDisable, ownerMinSvn,
             vendorMinSvn](auto&& handler) {
                // async_method_call uses move_only_function internally, so the
                // handler can be move-only.  Wrap in a concrete lambda with an
                // explicit (error_code, object_path) signature so
                // callable_traits::args_t can introspect the return type.
                auto sh = std::make_shared<std::decay_t<decltype(handler)>>(
                    std::forward<decltype(handler)>(handler));
                bus_->async_method_call(
                    [sh](boost::system::error_code ec,
                         sdbusplus::message::object_path path) {
                        (*sh)(ec, std::move(path));
                    },
                    kNsmService, objPath, kDotActionIntf, "DotCAKInstall",
                    cakAuthEnum, cakEcdsaKey, cakLmsKey, lakAuthEnum,
                    lakEcdsaKey, lakLmsKey, lockDisable, ownerMinSvn,
                    vendorMinSvn);
            },
            asio::use_awaitable);

        std::string asyncPath = asyncObjPath;
        lg2::info("DotCAKInstall async operation at {ASYNC}", "ASYNC",
                  asyncPath);

        std::string status = co_await pollAsyncStatus(asyncPath);
        if (status != kAsyncStatusSuccess)
        {
            throw std::runtime_error("DotCAKInstall failed for " + objPath +
                                     " with status: " + status);
        }
        donePaths.insert(objPath);
        lg2::info("DotCAKInstall succeeded for {PATH}", "PATH", objPath);
    }
}

asio::awaitable<std::string>
    DbusInstaller::pollAsyncStatus(const std::string& asyncPath)
{
    auto executor = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(config_.timeouts.installMs);
    asio::steady_timer timer(executor);

    while (true)
    {
        std::string status = co_await getPropertyAwaitable<std::string>(
            *bus_, kNsmService, asyncPath, kAsyncStatusIntf, kAsyncStatusProp);
        if (status != kAsyncStatusInProgress)
        {
            co_return status;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw std::runtime_error("D-Bus async operation timed out at " +
                                     asyncPath);
        }
        timer.expires_after(
            std::chrono::milliseconds(kDbusAsyncPollIntervalMs));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

// ---------------------------------------------------------------------------
// Post-install verification
// ---------------------------------------------------------------------------

asio::awaitable<void> DbusInstaller::waitForCakComplete()
{
    auto executor = co_await asio::this_coro::executor;
    int timeoutMs = config_.timeouts.installMs;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    lg2::info("Waiting for DOTCAKInitialization=Complete (timeout {TIMEOUT}ms)",
              "TIMEOUT", timeoutMs);

    asio::steady_timer timer(executor);
    while (true)
    {
        try
        {
            std::string state = co_await readCakInitState();
            lg2::debug("DOTCAKInitialization state: {STATE}", "STATE", state);

            if (state == "Complete")
            {
                lg2::info(
                    "DOTCAKInitialization=Complete — CAK installation verified");
                co_return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "CAK verification failed: DOTCAKInitialization did not "
                    "reach Complete within " +
                    std::to_string(timeoutMs / 1000) +
                    " seconds after L1 reset. Current state: " + state +
                    ". Make sure the correct CAK is being used.");
            }
            lg2::info("DOTCAKInitialization not yet Complete (state={STATE}), "
                      "retrying...",
                      "STATE", state);
        }
        catch (const boost::system::system_error& ex)
        {
            // Must come before std::runtime_error: system_error derives from it
            if (ex.code() == asio::error::operation_aborted)
                throw;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "CAK verification failed: could not read "
                    "DOTCAKInitialization within " +
                    std::to_string(timeoutMs / 1000) +
                    " seconds: " + ex.what());
            }
            lg2::debug("Failed to read DOTCAKInitialization: {ERROR}", "ERROR",
                       ex.what());
        }
        catch (const std::runtime_error&)
        {
            throw;
        }
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<std::string> DbusInstaller::readCakInitState()
{
    co_return co_await getPropertyAwaitable<std::string>(
        *bus_, kBootRawService, kBootCakPath, kBootProgressIntf,
        kBootProgressOemProp);
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

std::string DbusInstaller::toDbusAuthScheme(const std::string& scheme)
{
    if (scheme == "Ecdsa")
        return kCakSchemeEcdsa;
    if (scheme == "Hybrid")
        return std::string(kDotActionIntf) + ".KeyAuthScheme.Hybrid";
    throw std::runtime_error("Unknown auth scheme: " + scheme);
}
