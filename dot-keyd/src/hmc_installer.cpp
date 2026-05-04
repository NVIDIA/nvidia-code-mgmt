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

#include "hmc_installer.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <array>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace http = beast::http;

// ---------------------------------------------------------------------------
// Virtual override methods — small, delegate to private helpers
// ---------------------------------------------------------------------------

/** Returns true if CAK is already installed (DOTCAKInitialization=Complete). */
asio::awaitable<bool> HmcInstaller::shouldSkip()
{
    std::string state = co_await getCakInitState();
    bool skip = (state == "Complete");
    lg2::info("shouldSkip: DOTCAKInitialization={STATE} — {DECISION}", "STATE",
              state.empty() ? "<unavailable>" : state, "DECISION",
              skip ? "skipping (already complete)" : "proceeding with install");
    co_return skip;
}

/** Waits for DOTState=Uninitialized (DOTCAKInitialization=Waiting) before
 * proceeding, then delays 1s. */
asio::awaitable<void> HmcInstaller::doPreInstallCheck()
{
    lg2::info(
        "Pre-install check: waiting for DOTState=Uninitialized (DOTCAKInitialization=Waiting)");
    try
    {
        co_await waitForDotState("Uninitialized");
        lg2::info("Pre-install check passed");
    }
    catch (const CakInstallDeclinedException&)
    {
        throw;
    }
    catch (const std::exception& ex)
    {
        lg2::warning("Pre-install check failed: {ERROR}. Proceeding anyway.",
                     "ERROR", ex.what());
    }

    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(std::chrono::seconds(1));
    co_await timer.async_wait(asio::use_awaitable);
}

/** Sends the CAK payload to all install paths via Redfish, with retries. */
asio::awaitable<void> HmcInstaller::doInstall(const json& payload)
{
    std::string payloadStr = payload.dump();
    lg2::info("Starting Redfish CAK install to {NPATHS} path(s)", "NPATHS",
              config_.hmc->installPaths.size());
    std::set<std::string> donePaths;
    co_await retryInstall("Redfish", [&](int) -> asio::awaitable<void> {
        co_await sendToRemainingPaths(payloadStr, donePaths);
    });
    lg2::info("Redfish CAK install completed");
}

/** Waits for DOTCAKInitialization=Complete or DOTState=Volatile post-install.
 */
asio::awaitable<void> HmcInstaller::doVerify()
{
    int timeoutSecs = config_.timeouts.installMs / 1000;
    lg2::info("Post-install verification: waiting for DOTCAKInitialization="
              "Complete (timeout={TIMEOUT}s)",
              "TIMEOUT", timeoutSecs);
    bool failed = false;
    try
    {
        co_await waitForDotState("Volatile");
        lg2::info("Post-install verification passed");
    }
    catch (const std::exception& ex)
    {
        lg2::error("Post-install verification failed after {TIMEOUT}s: {ERROR}",
                   "TIMEOUT", timeoutSecs, "ERROR", ex.what());
        failed = true;
    }
    if (failed)
    {
        if (co_await logSbiosFmcRecoveryStatus())
            throw CpuInRecoveryException(
                "CPUs are in firmware recovery — the installed CAK "
                "does not match the key used to sign SBIOS");
        throw std::runtime_error(
            "CAK verification failed: system failed to boot within " +
            std::to_string(timeoutSecs) +
            " seconds after CAK installation. Make sure correct CAK is "
            "being used.");
    }
}

// ---------------------------------------------------------------------------
// CAK initialization state
// ---------------------------------------------------------------------------

/** Fetches DOTCAKInitialization from the HMC Redfish endpoint; returns "" on
 *  any error. */
asio::awaitable<std::string> HmcInstaller::getCakInitState()
{
    if (!config_.hmc)
    {
        co_return "";
    }
    try
    {
        std::string response = co_await httpRequest(
            http::verb::get, config_.hmc->dotCakInitPath, "");
        json data = json::parse(response);
        co_return data["Oem"]["Nvidia"].value("DOTCAKInitialization", "");
    }
    catch (const std::exception& ex)
    {
        lg2::debug("getCakInitState failed: {ERROR}", "ERROR", ex.what());
        co_return "";
    }
}

// ---------------------------------------------------------------------------
// State polling
// ---------------------------------------------------------------------------

/** Polls DOTState and DOTCAKInitialization until the expected state is reached
 *  or the configured timeout expires. */
asio::awaitable<void> HmcInstaller::waitForDotState(const std::string& expected)
{
    bool isBeforeInstall = (expected == "Uninitialized");
    bool isAfterInstall = (expected == "Volatile");
    std::string expectedCakInit = isBeforeInstall ? "Waiting" : "Complete";

    auto executor = co_await asio::this_coro::executor;
    auto startTime = std::chrono::steady_clock::now();
    auto deadline =
        startTime + std::chrono::milliseconds(config_.timeouts.installMs);
    asio::steady_timer timer(executor);

    while (true)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();

        auto [cakInitFetched, cakInitState] = co_await tryCakInitState();

        if (cakInitFetched)
        {
            lg2::info("DOTCAKInitialization={STATE} (elapsed={ELAPSED}ms, "
                      "expecting={EXPECTED})",
                      "STATE", cakInitState, "ELAPSED", elapsed, "EXPECTED",
                      expectedCakInit);
        }

        if (cakInitFetched && cakInitState == expectedCakInit)
        {
            lg2::info("DOTCAKInitialization={STATE} reached in {ELAPSED}ms",
                      "STATE", cakInitState, "ELAPSED", elapsed);
            co_return;
        }
        if (isBeforeInstall && cakInitFetched && cakInitState == "Complete")
        {
            throw CakInstallDeclinedException(
                "DOTCAKInitialization=Complete before install — "
                "CAK already installed");
        }
        if (isBeforeInstall && cakInitFetched)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "DOTCAKInitialization not " + expectedCakInit +
                    " (current: " + cakInitState + ") before timeout");
            }
            timer.expires_after(std::chrono::milliseconds(200));
            co_await timer.async_wait(asio::use_awaitable);
            continue;
        }

        auto [allMatch, dotStates] = co_await pollDotStates(expected);
        std::string statesStr = dotStatesToStr(dotStates);

        bool cakInitMatch = cakInitFetched && cakInitState == expectedCakInit;
        bool success = isAfterInstall ? (allMatch || cakInitMatch) : allMatch;

        if (isAfterInstall)
        {
            lg2::info("DOTState={STATES} elapsed={ELAPSED}ms", "STATES",
                      statesStr, "ELAPSED", elapsed);
        }

        if (success)
        {
            co_return;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw std::runtime_error(buildTimeoutError(
                expected, expectedCakInit, cakInitFetched, cakInitState,
                statesStr, isBeforeInstall, isAfterInstall));
        }
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

/** Wraps getCakInitState, returning a (fetched, state) pair. */
asio::awaitable<std::pair<bool, std::string>> HmcInstaller::tryCakInitState()
{
    std::string state = co_await getCakInitState();
    co_return std::make_pair(!state.empty(), state);
}

/** Fetches DOTState from each configured status path. */
asio::awaitable<std::pair<bool, std::vector<std::string>>>
    HmcInstaller::pollDotStates(const std::string& expected)
{
    std::vector<std::string> dotStates;
    bool allMatch = true;

    for (const auto& statusPath : config_.hmc->statusPaths)
    {
        try
        {
            std::string response =
                co_await httpRequest(http::verb::get, statusPath, "");
            json data = json::parse(response);
            std::string state = data.value("DOTState", "");
            dotStates.push_back(state);
            if (state != expected)
            {
                allMatch = false;
            }
        }
        catch (const std::exception&)
        {
            dotStates.push_back("");
            allMatch = false;
        }
    }
    co_return std::make_pair(allMatch, dotStates);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

/** POSTs the CAK payload to each install path not yet in donePaths. */
asio::awaitable<void>
    HmcInstaller::sendToRemainingPaths(const std::string& payloadStr,
                                       std::set<std::string>& donePaths)
{
    std::vector<std::string> remaining;
    for (const auto& path : config_.hmc->installPaths)
    {
        if (donePaths.find(path) == donePaths.end())
        {
            remaining.push_back(path);
        }
    }

    if (remaining.empty())
    {
        lg2::info("All CPUs already have CAK installed");
        co_return;
    }

    for (const auto& installPath : remaining)
    {
        lg2::info("Sending CAK installation request to {PATH}", "PATH",
                  installPath);
        try
        {
            std::string response =
                co_await httpRequest(http::verb::post, installPath, payloadStr);
            if (!response.empty())
            {
                lg2::info("Response from {PATH}: {BODY}", "PATH", installPath,
                          "BODY", response);
            }
            donePaths.insert(installPath);
            lg2::info("CAK installation succeeded for {PATH}", "PATH",
                      installPath);
        }
        catch (const CakInstallDeclinedException& ex)
        {
            lg2::info(
                "CAK already installed on {PATH} (409), skipping: {ERROR}",
                "PATH", installPath, "ERROR", ex.what());
            donePaths.insert(installPath);
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP (Beast)
// ---------------------------------------------------------------------------

/** Executes a single HTTP request to the HMC.  The host config field must be
 *  an IP address — async_resolve is intentionally avoided because on
 *  BOOST_ASIO_DISABLE_THREADS builds its cancellation handler attempts thread
 *  creation and throws EOPNOTSUPP. */
asio::awaitable<std::string> HmcInstaller::httpRequest(http::verb method,
                                                       const std::string& path,
                                                       const std::string& body)
{
    auto executor = co_await asio::this_coro::executor;
    auto [host, port] = parseHost();

    boost::system::error_code addrEc;
    auto addr = asio::ip::make_address(host, addrEc);
    if (addrEc)
    {
        throw std::runtime_error("HMC host must be an IP address: " + host +
                                 " (" + addrEc.message() + ")");
    }
    int portNum = 0;
    try
    {
        portNum = std::stoi(port);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("Invalid HMC port: " + port);
    }
    if (portNum <= 0 || portNum > 65535)
    {
        throw std::runtime_error("Invalid HMC port: " + port);
    }
    asio::ip::tcp::endpoint endpoint(addr, static_cast<uint16_t>(portNum));

    beast::tcp_stream stream(executor);
    stream.expires_after(std::chrono::milliseconds(kHmcRequestTimeoutMs));
    co_await stream.async_connect(endpoint, asio::use_awaitable);

    http::request<http::string_body> req{method, path, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "dot-keyd");
    if (!body.empty())
    {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    if (config_.hmc->username && config_.hmc->password)
    {
        req.set(http::field::authorization,
                "Basic " + base64Encode(*config_.hmc->username + ":" +
                                        *config_.hmc->password));
    }

    stream.expires_after(std::chrono::milliseconds(kHmcRequestTimeoutMs));
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buf, res, asio::use_awaitable);

    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

    if (res.result_int() == 409)
    {
        throw CakInstallDeclinedException(
            "HTTP 409 from " + path +
            ": CAK installation declined (already installed or policy rejection)");
    }
    if (res.result_int() >= 300)
    {
        lg2::warning("HTTP {STATUS} from {PATH}", "STATUS", res.result_int(),
                     "PATH", path);
        throw std::runtime_error("HTTP " + std::to_string(res.result_int()) +
                                 " from " + path);
    }
    co_return res.body();
}

// ---------------------------------------------------------------------------
// SBIOS FMC recovery status check
// ---------------------------------------------------------------------------

/** Queries HGX_SBIOS_FMC_N FirmwareInventory entries and logs an error if
 *  Health=Critical / State=StandbyOffline, which indicates CPUs booted into
 *  firmware recovery — most likely caused by a mismatched CAK.
 *  Returns true if any CPU is in recovery. */
asio::awaitable<bool> HmcInstaller::logSbiosFmcRecoveryStatus()
{
    static constexpr std::array<std::string_view, 2> kFmcIds = {
        "HGX_SBIOS_FMC_0", "HGX_SBIOS_FMC_1"};

    bool inRecovery = false;
    for (const auto& fmcId : kFmcIds)
    {
        const std::string path =
            "/redfish/v1/UpdateService/FirmwareInventory/" + std::string(fmcId);
        try
        {
            std::string response =
                co_await httpRequest(http::verb::get, path, "");
            json data = json::parse(response);

            std::string health =
                data.value("Status", json{}).value("Health", "");
            std::string state = data.value("Status", json{}).value("State", "");

            lg2::info(
                "SBIOS FMC {ID} post-reset status: Health={HEALTH} State={STATE}",
                "ID", fmcId, "HEALTH", health, "STATE", state);

            if (health == "Critical" && state == "StandbyOffline")
            {
                lg2::error(
                    "{ID} is in firmware recovery (Health=Critical, "
                    "State=StandbyOffline). The installed CAK likely does not "
                    "match the key used to sign SBIOS — verify the correct "
                    "CAK is being provisioned.",
                    "ID", fmcId);
                inRecovery = true;
            }
        }
        catch (const std::exception& ex)
        {
            lg2::warning(
                "Could not read SBIOS FMC recovery status for {ID}: {ERROR}",
                "ID", fmcId, "ERROR", ex.what());
        }
    }
    co_return inRecovery;
}

// ---------------------------------------------------------------------------
// L1 reset via HMC Redfish OEM action
// ---------------------------------------------------------------------------

/** Performs an L1 SW main reset by POSTing to the HMC Redfish OEM action
 *  endpoint.  bmcweb on the HMC forwards this request to snoopd (lpc-snooper)
 *  via D-Bus com.nvidia.L1Reset.Reset(), which issues the USB or I2C control
 *  transfer to the CPU.  Retries up to 5 times with 2-second delays. */
asio::awaitable<void> HmcInstaller::doL1Reset()
{
    // Build the Redfish path: <dotCakInitPath>/Oem/Nvidia/L1Reset
    // e.g. /redfish/v1/Systems/HGX_Baseboard_0/Oem/Nvidia/L1Reset
    const std::string l1ResetPath =
        config_.hmc->dotCakInitPath + "/Oem/Nvidia/L1Reset";

    lg2::info("L1 reset: posting to HMC Redfish endpoint {PATH}", "PATH",
              l1ResetPath);

    std::string lastError;
    for (int attempt = 1; attempt <= 5; ++attempt)
    {
        lg2::info("L1 reset attempt {ATTEMPT}/5: POST {PATH}", "ATTEMPT",
                  attempt, "PATH", l1ResetPath);
        try
        {
            std::string response =
                co_await httpRequest(http::verb::post, l1ResetPath, "{}");
            lg2::info("L1 reset Redfish POST succeeded on attempt {ATTEMPT}: "
                      "response={BODY}",
                      "ATTEMPT", attempt, "BODY",
                      response.empty() ? "(empty)" : response);
            co_return;
        }
        catch (const boost::system::system_error& ex)
        {
            if (ex.code() == asio::error::operation_aborted)
                throw;
            lastError = ex.what();
            lg2::warning("L1 reset attempt {ATTEMPT}/5 failed: {ERROR}",
                         "ATTEMPT", attempt, "ERROR", lastError);
        }
        catch (const std::exception& ex)
        {
            lastError = ex.what();
            lg2::warning("L1 reset attempt {ATTEMPT}/5 failed: {ERROR}",
                         "ATTEMPT", attempt, "ERROR", lastError);
        }

        if (attempt < 5)
        {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::seconds(2));
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    throw L1ResetFailedException(
        "L1 reset Redfish POST failed after 5 attempts: " + lastError);
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> HmcInstaller::parseHost() const
{
    const std::string& h = config_.hmc->host;
    if (!h.empty() && h.front() == '[')
    {
        auto close = h.find(']');
        if (close == std::string::npos)
        {
            throw std::runtime_error("Invalid HMC host (unclosed bracket): " +
                                     h);
        }
        std::string addr = h.substr(1, close - 1);
        std::string port = "80";
        if (close + 1 < h.size() && h[close + 1] == ':')
        {
            port = h.substr(close + 2);
        }
        return {addr, port};
    }
    auto colon = h.rfind(':');
    if (colon != std::string::npos)
    {
        return {h.substr(0, colon), h.substr(colon + 1)};
    }
    return {h, "80"};
}

std::string HmcInstaller::base64Encode(const std::string& input)
{
    static constexpr std::string_view chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3)
    {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        if (i + 1 < input.size())
            val |= static_cast<uint8_t>(input[i + 1]) << 8;
        if (i + 2 < input.size())
            val |= static_cast<uint8_t>(input[i + 2]);
        result += chars[(val >> 18) & 0x3f];
        result += chars[(val >> 12) & 0x3f];
        result += (i + 1 < input.size()) ? chars[(val >> 6) & 0x3f] : '=';
        result += (i + 2 < input.size()) ? chars[val & 0x3f] : '=';
    }
    return result;
}

std::string HmcInstaller::dotStatesToStr(const std::vector<std::string>& states)
{
    std::string out;
    for (size_t i = 0; i < states.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += "CPU" + std::to_string(i) + "=" +
               (states[i].empty() ? "<empty>" : states[i]);
    }
    return out;
}

std::string HmcInstaller::buildTimeoutError(
    const std::string& expected, const std::string& expectedCakInit,
    bool cakInitFetched, const std::string& cakInitState,
    const std::string& statesStr, bool isBeforeInstall, bool isAfterInstall)
{
    if (isBeforeInstall)
    {
        if (!cakInitFetched)
        {
            return "DOTCAKInitialization not available and DOTState not " +
                   expected +
                   " for both CPUs before timeout. Final DOTState: " +
                   statesStr;
        }
        return "DOTCAKInitialization not " + expectedCakInit +
               " (current: " + cakInitState + ") and DOTState not " + expected +
               " for both CPUs before timeout. Final DOTState: " + statesStr;
    }
    if (isAfterInstall)
    {
        return "Neither DOTState " + expected +
               " for both CPUs (current: " + statesStr +
               ") nor DOTCAKInitialization " + expectedCakInit + " (current: " +
               (cakInitFetched ? cakInitState : "<not available>") +
               ") before timeout";
    }
    return "DOTState not " + expected +
           " for both CPUs before timeout. Final DOTState: " + statesStr;
}
