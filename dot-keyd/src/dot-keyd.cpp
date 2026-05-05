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

/** Validates and writes a CAK payload received via D-Bus arguments to the
 *  on-disk key store. */
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

/** Returns the built-in default configuration. */
static Config getDefaultConfig()
{
    Config config;
    config.keyStorePath = kDefaultKeyStorePath;

    HmcConfig hmc;
    hmc.host = "172.31.13.251";
    config.hmc = hmc;

    return config;
}

/** Parses a JSON configuration object into a Config struct. */
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

    const auto& timeouts = data.value("timeouts", json::object());
    config.timeouts.installMs =
        timeouts.value("installMs", kDefaultInstallTimeoutMs);
    config.timeouts.dotCakInitMs =
        timeouts.value("dotCakInitMs", kDefaultDotCakInitTimeoutMs);
    if (config.timeouts.installMs <= 0)
        throw std::runtime_error("installMs must be positive");
    if (config.timeouts.dotCakInitMs <= 0)
        throw std::runtime_error("dotCakInitMs must be positive");

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
        config.hmc = std::nullopt;
    }
    if (data.contains("dbusDotPaths"))
    {
        config.dbusDotPaths =
            data.at("dbusDotPaths").get<std::vector<std::string>>();
    }

    return config;
}

/** Applies CLI argument overrides on top of a parsed Config. */
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
    return config;
}

/** Loads config from file (if present) and applies CLI overrides. */
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

/** Runs a single CAK provisioning attempt and returns an exit code. */
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

/** Returns the current CAK payload content for the CAKValue D-Bus property,
 *  or an empty string if readout is disabled, the file is absent, or invalid.
 */
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

/** Runs the CAK installer coroutine, updates the install status, and signals
 *  the CAKInstalled D-Bus property on completion or failure. */
static boost::asio::awaitable<void> runCakInstall(ServiceState svc)
{
    try
    {
        co_await svc.installer.run();
        svc.lastInstallStatus = "Installed";
        lg2::info("CAK installation completed successfully");
    }
    catch (const CakInstallDeclinedException& ex)
    {
        svc.lastInstallStatus = "Installed";
        lg2::info("CAK already installed on device: {MSG}", "MSG", ex.what());
    }
    catch (const CpuInRecoveryException& ex)
    {
        svc.lastInstallStatus = "CPU_In_Recovery";
        lg2::error("CPUs in firmware recovery: {ERROR}", "ERROR", ex.what());
    }
    catch (const CakInstallFailedException& ex)
    {
        svc.lastInstallStatus = "CAK_Install_Failed";
        lg2::error("CAK installation failed after all retries: {ERROR}",
                   "ERROR", ex.what());
    }
    catch (const L1ResetFailedException& ex)
    {
        svc.lastInstallStatus = "L1_Reset_Failed";
        lg2::error("L1 reset failed after all retries: {ERROR}", "ERROR",
                   ex.what());
    }
    catch (const boost::system::system_error& ex)
    {
        if (ex.code() == boost::asio::error::operation_aborted)
        {
            co_return;
        }
        std::string errorMsg = ex.what();
        if (errorMsg.find("CAK verification failed") != std::string::npos)
        {
            svc.lastInstallStatus = "Error";
            lg2::error("CAK verification failed: {ERROR}", "ERROR", errorMsg);
        }
        else
        {
            svc.lastInstallStatus = "NotInstalled";
            lg2::error("CAK installation failed: {ERROR}", "ERROR", errorMsg);
        }
    }
    catch (const std::exception& ex)
    {
        std::string errorMsg = ex.what();
        if (errorMsg.find("CAK verification failed") != std::string::npos)
        {
            svc.lastInstallStatus = "Error";
            lg2::error("CAK verification failed: {ERROR}", "ERROR", errorMsg);
        }
        else
        {
            svc.lastInstallStatus = "NotInstalled";
            lg2::error("CAK installation failed: {ERROR}", "ERROR", errorMsg);
        }
    }
    catch (...)
    {
        svc.lastInstallStatus = "NotInstalled";
        lg2::error("CAK installation failed: unknown exception");
    }
    try
    {
        svc.iface->signal_property("CAKInstalled");
    }
    catch (const std::exception& ex)
    {
        lg2::error("signal_property(CAKInstalled) threw: {ERROR}", "ERROR",
                   ex.what());
    }
    svc.installInProgress = false;
}

/** Starts the CAK install coroutine if one is not already running. */
static void triggerCakInstall(ServiceState svc)
{
    if (svc.installInProgress)
    {
        lg2::info("CAK install already in progress, skipping");
        return;
    }
    svc.installInProgress = true;
    boost::asio::co_spawn(svc.io, runCakInstall(svc),
                          boost::asio::bind_cancellation_slot(
                              svc.cancelInstall.slot(), boost::asio::detached));
}

/** Runs the D-Bus service loop: registers properties and methods, subscribes to
 *  host state changes, and runs the io_context with restart on unexpected
 * throws to guard against cancellation exceptions from
 * BOOST_ASIO_DISABLE_THREADS builds. */
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
                return;
            }
            const std::string* state = std::get_if<std::string>(&it->second);
            if (!state)
            {
                return;
            }

            lg2::info("HostState changed: {STATE}", "STATE", *state);

            if (*state == hostStateOn)
            {
                lg2::info("HostState → Running: triggering CAK installation");
                svc.hostIsOff = false;
                triggerCakInstall(svc);
            }
            else if (*state == hostStateOff)
            {
                lg2::info("HostState → Off: aborting install, resetting "
                          "CAKInstalled");
                svc.hostIsOff = true;
                try
                {
                    svc.cancelInstall.emit(
                        boost::asio::cancellation_type::terminal);
                }
                catch (...)
                {
                    lg2::warning("HostState → Off: cancelInstall.emit() threw "
                                 "(swallowed)");
                }
                svc.installInProgress = false;
                svc.lastInstallStatus = "NotInstalled";
                try
                {
                    svc.iface->signal_property("CAKInstalled");
                }
                catch (...)
                {
                    lg2::warning(
                        "HostState → Off: signal_property(CAKInstalled)"
                        " threw (swallowed)");
                }
            }
        });

    sdbusplus::asio::getProperty<std::string>(
        *bus, hostStateService, hostStatePath, hostStateInterface,
        hostStateProperty,
        [svc, hostStateOn, hostStateOff](const boost::system::error_code& ec,
                                         const std::string& state) {
            if (ec)
            {
                lg2::warning("Initial HostState read failed: {ERROR}", "ERROR",
                             ec.message());
                return;
            }
            lg2::info("Initial HostState: {STATE}", "STATE", state);
            if (state == hostStateOn)
            {
                svc.hostIsOff = false;
                triggerCakInstall(svc);
            }
            else if (state == hostStateOff)
            {
                svc.hostIsOff = true;
                svc.lastInstallStatus = "NotInstalled";
                svc.iface->signal_property("CAKInstalled");
            }
        });

    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait(
        [&](const boost::system::error_code&, int) { io.stop(); });

    int ioRestarts = 0;
    for (;;)
    {
        try
        {
            io.run();
            break;
        }
        catch (const std::exception& ex)
        {
            ++ioRestarts;
            lg2::error(
                "io_context handler threw (restart #{RESTARTS}): {ERROR}",
                "RESTARTS", ioRestarts, "ERROR", ex.what());
        }
        catch (...)
        {
            ++ioRestarts;
            lg2::error("io_context handler threw unknown exception "
                       "(restart #{RESTARTS})",
                       "RESTARTS", ioRestarts);
        }
    }
    return ExitCode::kSuccess;
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

/** Splits a whitespace-delimited string into a vector of tokens. */
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

/** Parses command-line arguments into an Args struct. */
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
