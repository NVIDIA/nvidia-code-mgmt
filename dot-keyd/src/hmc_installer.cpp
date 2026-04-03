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

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace http = beast::http;

// ---------------------------------------------------------------------------
// Virtual override methods — small, delegate to private helpers
// ---------------------------------------------------------------------------

asio::awaitable<bool> HmcInstaller::shouldSkip()
{
    lg2::info("shouldSkip: querying DOTCAKInitialization state");
    std::string state = co_await getCakInitState();
    lg2::info("shouldSkip: DOTCAKInitialization={STATE} — {DECISION}", "STATE",
              state.empty() ? "<unavailable>" : state, "DECISION",
              state == "Complete" ? "skipping (already complete)"
                                  : "proceeding with install");
    co_return state == "Complete";
}

asio::awaitable<void> HmcInstaller::doPreInstallCheck()
{
    lg2::info("doPreInstallCheck: starting pre-install DOT state check "
              "(expecting Uninitialized or DOTCAKInitialization=Waiting)");
    try
    {
        co_await waitForDotState("Uninitialized");
        lg2::info("doPreInstallCheck: DOT state verification passed");
    }
    catch (const std::exception& ex)
    {
        lg2::warning("doPreInstallCheck: DOT state check failed: {ERROR}. "
                     "Proceeding with install anyway.",
                     "ERROR", ex.what());
    }

    lg2::info("doPreInstallCheck: waiting 1s for HMC Redfish endpoint");
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(std::chrono::seconds(1));
    co_await timer.async_wait(asio::use_awaitable);
    lg2::info("doPreInstallCheck: done");
}

asio::awaitable<void> HmcInstaller::doInstall(const json& payload)
{
    std::string payloadStr = payload.dump();
    lg2::info("doInstall: starting Redfish CAK install, "
              "payload size={SIZE} bytes, install paths={NPATHS}",
              "SIZE", payloadStr.size(), "NPATHS",
              config_.hmc->installPaths.size());
    std::set<std::string> donePaths;
    co_await retryInstall("Redfish", [&](int attempt) -> asio::awaitable<void> {
        lg2::info("doInstall: retry-install callback, attempt={ATTEMPT}, "
                  "donePaths={NDONE}/{NTOTAL}",
                  "ATTEMPT", attempt, "NDONE", donePaths.size(), "NTOTAL",
                  config_.hmc->installPaths.size());
        co_await sendToRemainingPaths(payloadStr, donePaths);
    });
    lg2::info("doInstall: all install paths completed");
}

asio::awaitable<void> HmcInstaller::doVerify()
{
    int timeoutSecs = config_.timeouts.installMs / 1000;
    lg2::info("doVerify: waiting for DOTState=Volatile or "
              "DOTCAKInitialization=Complete (timeout={TIMEOUT}s)",
              "TIMEOUT", timeoutSecs);
    try
    {
        co_await waitForDotState("Volatile");
        lg2::info("doVerify: post-install DOT state verification passed");
    }
    catch (const std::exception& ex)
    {
        lg2::error("doVerify: post-install verification failed after "
                   "{TIMEOUT}s: {ERROR}",
                   "TIMEOUT", timeoutSecs, "ERROR", ex.what());
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

asio::awaitable<std::string> HmcInstaller::getCakInitState()
{
    if (!config_.hmc)
    {
        lg2::debug("getCakInitState: no HMC config, returning empty");
        co_return "";
    }
    lg2::debug("getCakInitState: GET {PATH}", "PATH",
               config_.hmc->dotCakInitPath);
    try
    {
        std::string response = co_await httpRequest(
            http::verb::get, config_.hmc->dotCakInitPath, "");
        json data = json::parse(response);
        std::string state =
            data["Oem"]["Nvidia"].value("DOTCAKInitialization", "");
        lg2::debug("getCakInitState: DOTCAKInitialization={STATE}", "STATE",
                   state.empty() ? "<not present in response>" : state);
        co_return state;
    }
    catch (const std::exception& ex)
    {
        lg2::debug("getCakInitState: request failed: {ERROR}", "ERROR",
                   ex.what());
        co_return "";
    }
}

// ---------------------------------------------------------------------------
// State polling
// ---------------------------------------------------------------------------

asio::awaitable<void> HmcInstaller::waitForDotState(const std::string& expected)
{
    bool isBeforeInstall = (expected == "Uninitialized");
    bool isAfterInstall = (expected == "Volatile");
    std::string expectedCakInit = isBeforeInstall ? "Waiting" : "Complete";
    int timeoutSecs = config_.timeouts.installMs / 1000;

    lg2::info("waitForDotState: waiting for DOTState={EXPECTED} "
              "(DOTCAKInitialization={CAKINIT_EXPECTED}, "
              "phase={PHASE}, timeout={TIMEOUT}s)",
              "EXPECTED", expected, "CAKINIT_EXPECTED", expectedCakInit,
              "PHASE", isBeforeInstall  ? "pre-install"
                       : isAfterInstall ? "post-install"
                                        : "other",
              "TIMEOUT", timeoutSecs);

    auto executor = co_await asio::this_coro::executor;
    auto startTime = std::chrono::steady_clock::now();
    auto deadline =
        startTime + std::chrono::milliseconds(config_.timeouts.installMs);
    asio::steady_timer timer(executor);
    int iteration = 0;

    while (true)
    {
        ++iteration;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();

        auto [cakInitFetched, cakInitState] = co_await tryCakInitState();

        lg2::info("waitForDotState[{ITER}] elapsed={ELAPSED}ms: "
                  "DOTCAKInitialization={CAKINIT} (fetched={FETCHED})",
                  "ITER", iteration, "ELAPSED", elapsed, "CAKINIT",
                  cakInitFetched ? cakInitState : "<unavailable>", "FETCHED",
                  cakInitFetched ? "yes" : "no");

        if (cakInitFetched && cakInitState == expectedCakInit)
        {
            lg2::info(
                "waitForDotState[{ITER}]: DOTCAKInitialization={STATE} matches "
                "expected — done (elapsed={ELAPSED}ms)",
                "ITER", iteration, "STATE", cakInitState, "ELAPSED", elapsed);
            co_return;
        }
        if (isBeforeInstall && cakInitFetched && cakInitState == "Complete")
        {
            lg2::info("waitForDotState[{ITER}]: DOTCAKInitialization=Complete "
                      "before install — CAK already installed, skipping "
                      "(elapsed={ELAPSED}ms)",
                      "ITER", iteration, "ELAPSED", elapsed);
            co_return;
        }
        if (isBeforeInstall && cakInitFetched)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "DOTCAKInitialization not " + expectedCakInit +
                    " (current: " + cakInitState + ") before timeout");
            }
            lg2::debug("waitForDotState[{ITER}]: DOTCAKInitialization={STATE}, "
                       "waiting 200ms",
                       "ITER", iteration, "STATE", cakInitState);
            timer.expires_after(std::chrono::milliseconds(200));
            co_await timer.async_wait(asio::use_awaitable);
            continue;
        }

        auto [allMatch, dotStates] = co_await pollDotStates(expected);
        std::string statesStr = dotStatesToStr(dotStates);

        bool cakInitMatch = cakInitFetched && cakInitState == expectedCakInit;
        bool success = isAfterInstall ? (allMatch || cakInitMatch) : allMatch;

        lg2::info("waitForDotState[{ITER}] elapsed={ELAPSED}ms: "
                  "DOTState={STATES} allMatch={ALLMATCH} "
                  "cakInitMatch={CAKINITMATCH} success={SUCCESS}",
                  "ITER", iteration, "ELAPSED", elapsed, "STATES", statesStr,
                  "ALLMATCH", allMatch ? "yes" : "no", "CAKINITMATCH",
                  cakInitMatch ? "yes" : "no", "SUCCESS",
                  success ? "yes" : "no");

        if (success)
        {
            lg2::info(
                "waitForDotState[{ITER}]: success — elapsed={ELAPSED}ms",
                "ITER", iteration, "ELAPSED", elapsed);
            co_return;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw std::runtime_error(buildTimeoutError(
                expected, expectedCakInit, cakInitFetched, cakInitState,
                statesStr, isBeforeInstall, isAfterInstall));
        }
        lg2::debug("waitForDotState[{ITER}]: not done yet, waiting 200ms",
                   "ITER", iteration);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<std::pair<bool, std::string>> HmcInstaller::tryCakInitState()
{
    std::string state = co_await getCakInitState();
    co_return std::make_pair(!state.empty(), state);
}

asio::awaitable<std::pair<bool, std::vector<std::string>>>
    HmcInstaller::pollDotStates(const std::string& expected)
{
    std::vector<std::string> dotStates;
    bool allMatch = true;

    for (const auto& statusPath : config_.hmc->statusPaths)
    {
        lg2::debug("pollDotStates: GET {PATH} (expecting DOTState={EXPECTED})",
                   "PATH", statusPath, "EXPECTED", expected);
        try
        {
            std::string response =
                co_await httpRequest(http::verb::get, statusPath, "");
            json data = json::parse(response);
            std::string state = data.value("DOTState", "");
            lg2::debug("pollDotStates: {PATH} → DOTState={STATE} "
                       "(match={MATCH})",
                       "PATH", statusPath, "STATE",
                       state.empty() ? "<not present>" : state, "MATCH",
                       state == expected ? "yes" : "no");
            dotStates.push_back(state);
            if (state != expected)
            {
                allMatch = false;
            }
        }
        catch (const std::exception& ex)
        {
            lg2::debug("pollDotStates: {PATH} → request failed: {ERROR}",
                       "PATH", statusPath, "ERROR", ex.what());
            dotStates.push_back("");
            allMatch = false;
        }
    }
    co_return std::make_pair(allMatch, dotStates);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

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

    lg2::info("sendToRemainingPaths: {NREMAIN} of {NTOTAL} paths pending "
              "(already done: {NDONE})",
              "NREMAIN", remaining.size(), "NTOTAL",
              config_.hmc->installPaths.size(), "NDONE", donePaths.size());

    if (remaining.empty())
    {
        lg2::info("sendToRemainingPaths: all paths already done, nothing to do");
        co_return;
    }

    for (const auto& installPath : remaining)
    {
        lg2::info("sendToRemainingPaths: POST {PATH}", "PATH", installPath);
        try
        {
            std::string response =
                co_await httpRequest(http::verb::post, installPath, payloadStr);
            lg2::info(
                "sendToRemainingPaths: POST {PATH} → success "
                "(response {BYTES} bytes)",
                "PATH", installPath, "BYTES", response.size());
            if (!response.empty())
            {
                lg2::info("sendToRemainingPaths: response body: {BODY}", "BODY",
                          response);
            }
            donePaths.insert(installPath);
        }
        catch (const CakInstallDeclinedException& ex)
        {
            // 409: already installed on this CPU — mark done and continue
            lg2::info(
                "sendToRemainingPaths: {PATH} → 409 (CAK already installed), "
                "marking done: {ERROR}",
                "PATH", installPath, "ERROR", ex.what());
            donePaths.insert(installPath);
        }
    }
    lg2::info("sendToRemainingPaths: done, total completed={NDONE}/{NTOTAL}",
              "NDONE", donePaths.size(), "NTOTAL",
              config_.hmc->installPaths.size());
}

// ---------------------------------------------------------------------------
// HTTP (Beast)
// ---------------------------------------------------------------------------

asio::awaitable<std::string> HmcInstaller::httpRequest(http::verb method,
                                                       const std::string& path,
                                                       const std::string& body)
{
    auto executor = co_await asio::this_coro::executor;
    auto [host, port] = parseHost();

    // Parse host as an IP address directly rather than using async_resolve.
    // On platforms built with BOOST_ASIO_DISABLE_THREADS, the resolver's
    // cancellation handler internally attempts thread creation, which throws
    // system:95 (EOPNOTSUPP) during terminal cancellation.  The HMC host is
    // always an IP address in practice, so DNS resolution is not needed.
    boost::system::error_code addrEc;
    auto addr = asio::ip::make_address(host, addrEc);
    if (addrEc)
    {
        throw std::runtime_error("HMC host must be an IP address: " + host +
                                 " (" + addrEc.message() + ")");
    }
    int portNum = std::stoi(port);
    if (portNum <= 0 || portNum > 65535)
    {
        throw std::runtime_error("Invalid HMC port: " + port);
    }
    asio::ip::tcp::endpoint endpoint(addr, static_cast<uint16_t>(portNum));

    std::string methodStr(method == http::verb::get    ? "GET"
                          : method == http::verb::post ? "POST"
                                                       : "OTHER");
    lg2::debug("httpRequest: {METHOD} http://{HOST}:{PORT}{PATH} "
               "(timeout={TIMEOUT}ms, body={BODYSIZE}B)",
               "METHOD", methodStr, "HOST", host, "PORT", port, "PATH", path,
               "TIMEOUT", kHmcRequestTimeoutMs, "BODYSIZE", body.size());

    beast::tcp_stream stream(executor);
    stream.expires_after(std::chrono::milliseconds(kHmcRequestTimeoutMs));
    lg2::debug("httpRequest: connecting to {HOST}:{PORT}", "HOST", host, "PORT",
               port);
    co_await stream.async_connect(endpoint, asio::use_awaitable);
    lg2::debug("httpRequest: connected to {HOST}:{PORT}", "HOST", host, "PORT",
               port);

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
    lg2::debug("httpRequest: sending {METHOD} {PATH}", "METHOD", methodStr,
               "PATH", path);
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    lg2::debug("httpRequest: reading response for {METHOD} {PATH}", "METHOD",
               methodStr, "PATH", path);
    co_await http::async_read(stream, buf, res, asio::use_awaitable);

    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    if (ec)
    {
        lg2::debug("httpRequest: socket shutdown warning: {ERROR}", "ERROR",
                   ec.message());
    }

    lg2::debug("httpRequest: {METHOD} {PATH} → HTTP {STATUS} "
               "({BYTES} bytes body)",
               "METHOD", methodStr, "PATH", path, "STATUS",
               res.result_int(), "BYTES", res.body().size());

    if (res.result_int() == 409)
    {
        throw CakInstallDeclinedException(
            "HTTP 409 from " + path +
            ": CAK installation declined (already installed or policy rejection)");
    }
    if (res.result_int() >= 300)
    {
        lg2::warning("httpRequest: {METHOD} {PATH} → HTTP {STATUS} (error)",
                     "METHOD", methodStr, "PATH", path, "STATUS",
                     res.result_int());
        throw std::runtime_error("HTTP " + std::to_string(res.result_int()) +
                                 " from " + path);
    }
    co_return res.body();
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> HmcInstaller::parseHost() const
{
    const std::string& h = config_.hmc->host;
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
