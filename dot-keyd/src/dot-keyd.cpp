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

#include "cak.hpp"
#include "config.hpp"
#include "dot_installer.hpp"

#include <getopt.h>

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// D-Bus key-store write method (called by installCak2BmcFs)
// ---------------------------------------------------------------------------

static void
    storeCakFromDbusArgs(const Config& config,
                         const std::tuple<std::string, std::string>& cakKey,
                         bool lockDisable, int64_t vendorMinimumSecurityVersion,
                         int64_t ownerMinimumSecurityVersion, bool hasLakKey,
                         const std::tuple<std::string, std::string>& lakKey)
{
    json payload;
    payload["CAKKey"] = {
        {"AuthenticationScheme", std::get<0>(cakKey)},
        {"ECDSAKey", std::get<1>(cakKey)},
    };
    payload["LockDisable"] = lockDisable;
    payload["VendorMinimumSecurityVersion"] = vendorMinimumSecurityVersion;
    payload["OwnerMinimumSecurityVersion"] = ownerMinimumSecurityVersion;

    if (hasLakKey)
    {
        payload["LAKKey"] = {
            {"AuthenticationScheme", std::get<0>(lakKey)},
            {"ECDSAKey", std::get<1>(lakKey)},
        };
    }

    validateDotPayload(payload);

    std::string cakEcdsaKey = payload["CAKKey"]["ECDSAKey"].get<std::string>();
    normalizePemKeyFormat(cakEcdsaKey);
    payload["CAKKey"]["ECDSAKey"] = cakEcdsaKey;

    if (hasLakKey)
    {
        std::string lakEcdsaKey =
            payload["LAKKey"]["ECDSAKey"].get<std::string>();
        normalizePemKeyFormat(lakEcdsaKey);
        payload["LAKKey"]["ECDSAKey"] = lakEcdsaKey;
    }

    std::string normalizedPayload =
        payload.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
    validateCakBytes(cakEcdsaKey, config.maxCakBytes);
    atomicWrite(payloadPath(config.keyStorePath), normalizedPayload);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static Config getDefaultConfig()
{
    Config config;
    config.keyStorePath = kDefaultKeyStorePath;

    HmcConfig hmc;
    hmc.host = "172.31.13.251";
    config.hmc = hmc;

    return config;
}

static Config parseConfig(const json& data)
{
    Config config = getDefaultConfig();
    if (data.contains("keyStorePath"))
    {
        config.keyStorePath = data.at("keyStorePath").get<std::string>();
    }
    else if (data.contains("cakStorePath"))
    {
        config.keyStorePath = data.at("cakStorePath").get<std::string>();
    }

    if (data.contains("hmc"))
    {
        HmcConfig hmc;
        const auto& hmcData = data.at("hmc");
        hmc.host = hmcData.at("host").get<std::string>();
        if (hmcData.contains("username"))
        {
            hmc.username = hmcData.at("username").get<std::string>();
        }
        if (hmcData.contains("password"))
        {
            hmc.password = hmcData.at("password").get<std::string>();
        }
        hmc.verifyTls = hmcData.value("verifyTls", false);
        if (hmcData.contains("installPaths"))
        {
            hmc.installPaths =
                hmcData.at("installPaths").get<std::vector<std::string>>();
        }
        if (hmcData.contains("statusPaths"))
        {
            hmc.statusPaths =
                hmcData.at("statusPaths").get<std::vector<std::string>>();
        }
        if (hmcData.contains("dotCakInitPath"))
        {
            hmc.dotCakInitPath =
                hmcData.at("dotCakInitPath").get<std::string>();
        }
        config.hmc = hmc;
    }

    if (data.contains("hmclessExec"))
    {
        HmclessExecConfig exec;
        const auto& execData = data.at("hmclessExec");
        exec.path = execData.at("path").get<std::string>();
        exec.args = execData.value("args", std::vector<std::string>{});
        config.hmclessExec = exec;
    }

    if (data.contains("l1Reset"))
    {
        const auto& l1Reset = data.at("l1Reset");
        if (l1Reset.contains("i2cBus"))
        {
            config.l1Reset.i2cBus = l1Reset.at("i2cBus").get<int>();
        }
        if (l1Reset.contains("i2cAddr"))
        {
            config.l1Reset.i2cAddr = l1Reset.at("i2cAddr").get<std::string>();
        }
    }

    const auto& timeouts = data.value("timeouts", json::object());
    config.timeouts.installMs =
        timeouts.value("installMs", kDefaultInstallTimeoutMs);
    config.timeouts.dotCakInitMs =
        timeouts.value("dotCakInitMs", kDefaultDotCakInitTimeoutMs);

    config.maxCakBytes = data.value("maxCakBytes", kDefaultCakMaxBytes);
    config.allowCakReadout = data.value("allowCakReadout", true);
    if (data.contains("minimumSecurityVersion"))
    {
        config.minimumSecurityVersion =
            data.at("minimumSecurityVersion").get<int>();
    }
    config.cakInstallRetries =
        data.value("cakInstallRetries", kDefaultCakInstallRetries);
    config.useDefaultDbus = data.value("useDefaultDbus", false);
    if (config.useDefaultDbus)
    {
        // useDefaultDbus mode calls nsmd directly; HMC config is not applicable
        // and must not be used even if set by default.
        config.hmc = std::nullopt;
    }
    if (data.contains("dbusDotPaths"))
    {
        config.dbusDotPaths =
            data.at("dbusDotPaths").get<std::vector<std::string>>();
    }

    return config;
}

static Config applyCliOverrides(Config config, const Args& args)
{
    if (args.keyStorePath)
    {
        config.keyStorePath = *args.keyStorePath;
    }
    if (args.hmclessExec)
    {
        HmclessExecConfig exec;
        exec.path = *args.hmclessExec;
        exec.args = args.hmclessArgs;
        config.hmclessExec = exec;
    }
    if (args.i2cBus)
    {
        config.l1Reset.i2cBus = *args.i2cBus;
    }
    if (args.i2cAddr)
    {
        config.l1Reset.i2cAddr = *args.i2cAddr;
    }
    return config;
}

static Config resolveConfig(const Args& args)
{
    Config config = getDefaultConfig();

    if (!args.configPath.empty() && fs::exists(args.configPath))
    {
        try
        {
            json data = json::parse(readFile(args.configPath));
            config = parseConfig(data);
        }
        catch (const std::exception& ex)
        {
            throw std::runtime_error("Failed to parse config file: " +
                                     std::string(ex.what()));
        }
    }

    return applyCliOverrides(config, args);
}

// ---------------------------------------------------------------------------
// One-shot install flow
// ---------------------------------------------------------------------------

static int installFlow(const Args& args)
{
    Config config;
    try
    {
        config = resolveConfig(args);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "config error: " << ex.what() << "\n";
        return ExitCode::kInvalidArgs;
    }

    try
    {
        ensureDir(config.keyStorePath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "storage error: " << ex.what() << "\n";
        return ExitCode::kStorageError;
    }

    if (args.cakPath)
    {
        try
        {
            storeCak(config, readFile(*args.cakPath));
        }
        catch (const std::exception& ex)
        {
            std::cerr << "invalid CAK: " << ex.what() << "\n";
            return ExitCode::kInvalidCak;
        }
    }
    else if (!fs::exists(payloadPath(config.keyStorePath)))
    {
        std::cerr << "CAK is required (no stored CAK found)\n";
        return ExitCode::kInvalidArgs;
    }

    boost::asio::io_context io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    auto installer = createInstaller(config, bus);

    int result = ExitCode::kSuccess;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> { co_await installer->run(); },
        [&](std::exception_ptr ep) {
            if (ep)
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "provisioning error: " << ex.what() << "\n";
                    result = ExitCode::kProvisioningError;
                }
            }
            io.stop();
        });

    auto start = std::chrono::steady_clock::now();
    try
    {
        io.run();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "io_context error: " << ex.what() << "\n";
        return ExitCode::kProvisioningError;
    }

    if (result != ExitCode::kSuccess)
    {
        return result;
    }
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    if (elapsedMs > config.timeouts.installMs)
    {
        std::cerr << "install timeout exceeded\n";
        return ExitCode::kProvisioningError;
    }
    return ExitCode::kSuccess;
}

// ---------------------------------------------------------------------------
// D-Bus service mode — shared state and worker helpers
// ---------------------------------------------------------------------------

struct ServiceState
{
    DotInstaller& installer;
    const Config& config;
    boost::asio::io_context& io;
    bool& hostIsOff;
    bool& installInProgress;
    std::string& lastInstallStatus;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    boost::asio::cancellation_signal& cancelInstall;
};

static std::string readCakValue(const Config& config)
{
    if (!config.allowCakReadout)
    {
        return {};
    }
    try
    {
        fs::path payloadFile = payloadPath(config.keyStorePath);
        if (!fs::exists(payloadFile))
        {
            return {};
        }
        std::string content = readFile(payloadFile);
        try
        {
            json payload = json::parse(content);
            validateDotPayload(payload);
            return content;
        }
        catch (const json::parse_error& ex)
        {
            lg2::error("CAKValue: invalid JSON in payload file: {ERROR}",
                       "ERROR", ex.what());
            return {};
        }
        catch (const std::exception& ex)
        {
            lg2::error("CAKValue: invalid payload structure: {ERROR}", "ERROR",
                       ex.what());
            return {};
        }
    }
    catch (const std::exception& ex)
    {
        lg2::error("CAKValue: failed to read payload file: {ERROR}", "ERROR",
                   ex.what());
        return {};
    }
}

static boost::asio::awaitable<void> runCakInstall(ServiceState svc)
{
    lg2::info("runCakInstall: starting, current status={STATUS} "
              "hostIsOff={HOSTOFF}",
              "STATUS", svc.lastInstallStatus, "HOSTOFF",
              svc.hostIsOff ? "yes" : "no");
    try
    {
        co_await svc.installer.run();
        svc.lastInstallStatus = "Installed";
        lg2::info("runCakInstall: installer.run() completed successfully → "
                  "status=Installed");
    }
    catch (const CakInstallDeclinedException& ex)
    {
        svc.lastInstallStatus = "NotApplicable";
        lg2::warning("runCakInstall: CakInstallDeclinedException → "
                     "status=NotApplicable: {ERROR}",
                     "ERROR", ex.what());
    }
    catch (const boost::system::system_error& ex)
    {
        lg2::info("runCakInstall: boost::system::system_error caught: "
                  "code={CODE} message={ERROR}",
                  "CODE", ex.code().value(), "ERROR", ex.what());
        if (ex.code() == boost::asio::error::operation_aborted)
        {
            lg2::info("runCakInstall: operation_aborted — host went off, "
                      "status already set by Off handler, co_return");
            co_return;
        }
        std::string errorMsg = ex.what();
        if (errorMsg.find("CAK verification failed") != std::string::npos)
        {
            svc.lastInstallStatus = "Error";
            lg2::error("runCakInstall: system_error, CAK verification failed "
                       "→ status=Error: {ERROR}",
                       "ERROR", errorMsg);
        }
        else
        {
            svc.lastInstallStatus = "NotInstalled";
            lg2::error("runCakInstall: system_error → status=NotInstalled: "
                       "{ERROR}",
                       "ERROR", errorMsg);
        }
    }
    catch (const std::exception& ex)
    {
        std::string errorMsg = ex.what();
        lg2::info("runCakInstall: std::exception caught: {ERROR}", "ERROR",
                  errorMsg);
        if (errorMsg.find("CAK verification failed") != std::string::npos)
        {
            svc.lastInstallStatus = "Error";
            lg2::error(
                "runCakInstall: CAK verification failed → status=Error: "
                "{ERROR}",
                "ERROR", errorMsg);
        }
        else
        {
            svc.lastInstallStatus = "NotInstalled";
            lg2::error("runCakInstall: exception → status=NotInstalled: "
                       "{ERROR}",
                       "ERROR", errorMsg);
        }
    }
    catch (...)
    {
        // Catch-all: prevents std::terminate via detached co_spawn if an
        // unexpected exception type (e.g. boost::wrapexcept wrapping a
        // system_error with EOPNOTSUPP from disabled-thread cancellation)
        // somehow escapes the typed catch blocks above.
        svc.lastInstallStatus = "NotInstalled";
        lg2::error("runCakInstall: unknown exception type caught (not "
                   "std::exception) → status=NotInstalled");
    }
    lg2::info("runCakInstall: finished, final status={STATUS}, signaling "
              "CAKInstalled property",
              "STATUS", svc.lastInstallStatus);
    try
    {
        svc.iface->signal_property("CAKInstalled");
    }
    catch (const std::exception& ex)
    {
        lg2::error("runCakInstall: signal_property(CAKInstalled) threw: "
                   "{ERROR}",
                   "ERROR", ex.what());
    }
    svc.installInProgress = false;
    lg2::info("runCakInstall: installInProgress=false, coroutine exiting");
}

static void triggerCakInstall(ServiceState svc)
{
    lg2::info("triggerCakInstall: called, installInProgress={INPROG} "
              "hostIsOff={HOSTOFF} lastStatus={STATUS}",
              "INPROG", svc.installInProgress ? "yes" : "no", "HOSTOFF",
              svc.hostIsOff ? "yes" : "no", "STATUS", svc.lastInstallStatus);
    if (svc.installInProgress)
    {
        lg2::info("triggerCakInstall: install already in progress, skipping");
        return;
    }
    lg2::info("triggerCakInstall: spawning runCakInstall coroutine");
    svc.installInProgress = true;
    boost::asio::co_spawn(svc.io, runCakInstall(svc),
                          boost::asio::bind_cancellation_slot(
                              svc.cancelInstall.slot(), boost::asio::detached));
}

static int runService(const Args& args)
{
    Config config;
    try
    {
        config = resolveConfig(args);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "config error: " << ex.what() << "\n";
        return ExitCode::kInvalidArgs;
    }

    boost::asio::io_context io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    bus->request_name("xyz.openbmc_project.Security.DOT");
    sdbusplus::asio::object_server server(bus);
    auto iface = server.add_interface("/xyz/openbmc_project/security/dot",
                                      "xyz.openbmc_project.Security.DOT");

    auto installer = createInstaller(config, bus);

    std::string lastInstallStatus = "NotInstalled";
    bool installInProgress = false;
    bool hostIsOff = false;
    boost::asio::cancellation_signal cancelInstall;
    ServiceState svc{
        *installer,        config, io,           hostIsOff, installInProgress,
        lastInstallStatus, iface,  cancelInstall};

    iface->register_property_r<bool>(
        "Stored", sdbusplus::vtable::property_::emits_change,
        [&config](const bool&) {
            return fs::exists(payloadPath(config.keyStorePath));
        });
    iface->register_property_r<std::string>(
        "CAKValue", sdbusplus::vtable::property_::emits_change,
        [&config](const std::string&) { return readCakValue(config); });
    iface->register_property_r<std::string>(
        "KeyStorePath", sdbusplus::vtable::property_::emits_change,
        [&config](const std::string&) { return config.keyStorePath.string(); });
    iface->register_property_r<std::string>(
        "CAKInstalled", sdbusplus::vtable::property_::emits_change,
        [&lastInstallStatus](const std::string&) { return lastInstallStatus; });
    iface->register_property_r<int>(
        "CAKVerificationTimeoutInSeconds",
        sdbusplus::vtable::property_::emits_change,
        [&config](const int&) { return config.timeouts.installMs / 1000; });

    iface->register_method("InstallCak", [svc]() { triggerCakInstall(svc); });
    iface->register_method(
        "installCak2BmcFs",
        [&config, iface](const std::tuple<std::string, std::string>& cakKey,
                         const bool& lockDisable,
                         const int64_t& vendorMinimumSecurityVersion,
                         const int64_t& ownerMinimumSecurityVersion,
                         const bool& hasLakKey,
                         const std::tuple<std::string, std::string>& lakKey) {
            try
            {
                storeCakFromDbusArgs(
                    config, cakKey, lockDisable, vendorMinimumSecurityVersion,
                    ownerMinimumSecurityVersion, hasLakKey, lakKey);
                iface->signal_property("Stored");
                return std::string{"Success"};
            }
            catch (const std::exception& ex)
            {
                return std::string{"Error: "} + ex.what();
            }
        });
    iface->register_method("DeleteCak", [&config, iface]() {
        try
        {
            deleteCak(config);
            iface->signal_property("Stored");
            return std::string{"Success"};
        }
        catch (const std::exception& ex)
        {
            return std::string{"Error: "} + ex.what();
        }
    });

    iface->initialize();

    constexpr const char* hostStateService = "xyz.openbmc_project.State.Host";
    constexpr const char* hostStatePath = "/xyz/openbmc_project/state/host0";
    constexpr const char* hostStateInterface = "xyz.openbmc_project.State.Host";
    constexpr const char* hostStateProperty = "CurrentHostState";
    constexpr const char* hostStateOn =
        "xyz.openbmc_project.State.Host.HostState.Running";
    constexpr const char* hostStateOff =
        "xyz.openbmc_project.State.Host.HostState.Off";

    auto hostStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*bus),
        sdbusplus::bus::match::rules::propertiesChanged(hostStatePath,
                                                        hostStateInterface),
        [svc, bus](sdbusplus::message_t& msg) {
            std::string interface;
            std::map<std::string, std::variant<std::string>> properties;
            msg.read(interface, properties);

            auto it = properties.find(hostStateProperty);
            if (it == properties.end())
            {
                lg2::debug("HostState propertiesChanged: no CurrentHostState "
                           "in message, ignoring");
                return;
            }
            const std::string* state = std::get_if<std::string>(&it->second);
            if (!state)
            {
                lg2::debug("HostState propertiesChanged: CurrentHostState is "
                           "not a string, ignoring");
                return;
            }

            lg2::info("HostState propertiesChanged: new state={STATE} "
                      "(installInProgress={INPROG} lastStatus={STATUS})",
                      "STATE", *state, "INPROG",
                      svc.installInProgress ? "yes" : "no", "STATUS",
                      svc.lastInstallStatus);

            if (*state == hostStateOn)
            {
                lg2::info("HostState → Running: triggering CAK installation");
                svc.hostIsOff = false;
                triggerCakInstall(svc);
            }
            else if (*state == hostStateOff)
            {
                lg2::info(
                    "HostState → Off: resetting CAKInstalled=NotInstalled, "
                    "aborting any in-progress installation "
                    "(installInProgress={INPROG})",
                    "INPROG", svc.installInProgress ? "yes" : "no");
                svc.hostIsOff = true;
                // Use catch(...) rather than catch(std::exception): on some
                // Boost/ABI combinations, boost::wrapexcept<system_error> is
                // not matched by catch(const std::exception&).  With
                // BOOST_ASIO_DISABLE_THREADS, cancelling an in-progress
                // async operation may internally attempt thread creation and
                // throw system:95 (EOPNOTSUPP).
                lg2::info("HostState → Off: emitting terminal cancellation");
                try
                {
                    svc.cancelInstall.emit(
                        boost::asio::cancellation_type::terminal);
                    lg2::info("HostState → Off: terminal cancellation emitted");
                }
                catch (...)
                {
                    lg2::warning(
                        "HostState → Off: cancelInstall.emit() threw "
                        "(exception swallowed) — install coroutine will exit "
                        "when its current operation completes");
                }
                svc.installInProgress = false;
                svc.lastInstallStatus = "NotInstalled";
                lg2::info("HostState → Off: signaling CAKInstalled=NotInstalled");
                try
                {
                    svc.iface->signal_property("CAKInstalled");
                }
                catch (...)
                {
                    lg2::warning(
                        "HostState → Off: signal_property(CAKInstalled) threw "
                        "(exception swallowed)");
                }
                lg2::info("HostState → Off: handler complete");
            }
            else
            {
                lg2::info("HostState propertiesChanged: unrecognised state "
                          "{STATE}, ignoring",
                          "STATE", *state);
            }
        });

    sdbusplus::asio::getProperty<std::string>(
        *bus, hostStateService, hostStatePath, hostStateInterface,
        hostStateProperty,
        [svc, hostStateOn, hostStateOff](const boost::system::error_code& ec,
                                         const std::string& state) {
            if (ec)
            {
                lg2::warning("Initial HostState read failed: {ERROR}. "
                             "Will rely on propertiesChanged match for state.",
                             "ERROR", ec.message());
                return;
            }
            lg2::info("Initial HostState: {STATE}", "STATE", state);
            if (state == hostStateOn)
            {
                lg2::info("Initial HostState is Running — host is already up, "
                          "NOT triggering install (waiting for next On event)");
                svc.hostIsOff = false;
            }
            else if (state == hostStateOff)
            {
                lg2::info("Initial HostState is Off — setting hostIsOff=true, "
                          "CAKInstalled=NotInstalled");
                svc.hostIsOff = true;
                svc.lastInstallStatus = "NotInstalled";
                svc.iface->signal_property("CAKInstalled");
            }
            else
            {
                lg2::info("Initial HostState is {STATE} (neither Running nor "
                          "Off), no action",
                          "STATE", state);
            }
        });

    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait(
        [&](const boost::system::error_code&, int) { io.stop(); });
    // Run the io_context in a resilient loop: if a handler throws (e.g. from
    // Boost.Asio's async cancellation machinery when
    // BOOST_ASIO_DISABLE_THREADS is set), log and restart rather than exiting.
    // io_context::run() can be re-entered after a throw; pending completions
    // remain queued and will be processed on the next iteration.
    lg2::info("runService: entering io_context run loop");
    int ioRestarts = 0;
    for (;;)
    {
        try
        {
            io.run();
            lg2::info("runService: io_context run() returned normally "
                      "(io.stop() called), restarts={RESTARTS}",
                      "RESTARTS", ioRestarts);
            break; // ran out of work — io.stop() called (SIGTERM/SIGINT)
        }
        catch (const std::exception& ex)
        {
            ++ioRestarts;
            lg2::error(
                "runService: io_context handler threw (restart #{RESTARTS}): "
                "{ERROR}. Re-entering run loop.",
                "RESTARTS", ioRestarts, "ERROR", ex.what());
        }
        catch (...)
        {
            ++ioRestarts;
            lg2::error(
                "runService: io_context handler threw unknown exception "
                "(restart #{RESTARTS}). Re-entering run loop.",
                "RESTARTS", ioRestarts);
        }
    }
    lg2::info("runService: exiting, total io_context restarts={RESTARTS}",
              "RESTARTS", ioRestarts);
    return ExitCode::kSuccess;
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

static std::vector<std::string> splitArgs(const std::string& input)
{
    std::istringstream stream(input);
    std::vector<std::string> out;
    std::string token;
    while (stream >> token)
    {
        out.push_back(token);
    }
    return out;
}

static Args parseArgs(int argc, char** argv)
{
    Args args;
    const char* const shortOpts = "";
    const option longOpts[] = {
        {"config", required_argument, nullptr, 0},
        {"install", no_argument, nullptr, 0},
        {"delete", no_argument, nullptr, 0},
        {"cak", required_argument, nullptr, 0},
        {"key-store-path", required_argument, nullptr, 0},
        {"hmcless-exec", required_argument, nullptr, 0},
        {"hmcless-args", required_argument, nullptr, 0},
        {"i2c-bus", required_argument, nullptr, 0},
        {"i2c-addr", required_argument, nullptr, 0},
        {nullptr, 0, nullptr, 0},
    };
    int longIndex = 0;
    while (true)
    {
        int opt = ::getopt_long(argc, argv, shortOpts, longOpts, &longIndex);
        if (opt == -1)
        {
            break;
        }
        if (opt != 0)
        {
            continue;
        }
        std::string name = longOpts[longIndex].name;
        if (name == "config")
        {
            args.configPath = optarg;
        }
        else if (name == "install")
        {
            args.install = true;
        }
        else if (name == "delete")
        {
            args.deleteCak = true;
        }
        else if (name == "cak")
        {
            args.cakPath = optarg;
        }
        else if (name == "key-store-path")
        {
            args.keyStorePath = optarg;
        }
        else if (name == "hmcless-exec")
        {
            args.hmclessExec = optarg;
        }
        else if (name == "hmcless-args")
        {
            args.hmclessArgs = splitArgs(optarg);
        }
        else if (name == "i2c-bus")
        {
            try
            {
                args.i2cBus = std::stoi(optarg);
            }
            catch (const std::exception&)
            {
                std::cerr << "invalid --i2c-bus value: " << optarg << "\n";
                std::exit(ExitCode::kInvalidArgs);
            }
        }
        else if (name == "i2c-addr")
        {
            args.i2cAddr = optarg;
        }
    }
    return args;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    Args args = parseArgs(argc, argv);
    if (args.deleteCak)
    {
        try
        {
            Config config = resolveConfig(args);
            deleteCak(config);
            return ExitCode::kSuccess;
        }
        catch (const std::exception& ex)
        {
            std::cerr << "delete error: " << ex.what() << "\n";
            return ExitCode::kStorageError;
        }
    }
    if (args.install)
    {
        return installFlow(args);
    }
    return runService(args);
}
