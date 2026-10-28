// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "mst_recovery.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <regex>
#include <thread>
#include <utility>

namespace mst
{

namespace
{

constexpr auto levelPrefix = "xyz.openbmc_project.Logging.Entry.Level.";
constexpr auto resolutionKey = "xyz.openbmc_project.Logging.Entry.Resolution";
constexpr auto deviceStateResolution =
    "Check device state and try recovery again.";
constexpr auto notProvisionedResolution = "Return to NVIDIA for provisioning.";
constexpr auto mcuVidPidNormal = "0955:cf11";
constexpr auto mcuVidPidIsp = "0955:cf1d";
constexpr unsigned usbAttempts = 3;
constexpr uint64_t holdSettleMs = 100;
// Mst flow property, I2C and GPIO attempts; Hold flow EM lookups
constexpr unsigned readAttempts = 10;
constexpr uint64_t retryDelayMs = 1000;
constexpr uint64_t resetPulseMs = 500;
constexpr uint64_t resetSettleMs = 500;
constexpr uint64_t gisPollTimeoutMs = 20000;
constexpr uint64_t gisPollIntervalMs = 2000;
constexpr uint64_t burnAttempts = 3;
constexpr std::string_view burnRetryPattern = "Can't find device id";
constexpr uint64_t burnRetryDelayMs = 3000;
constexpr uint64_t bootDoneSettleMs = 1000;
constexpr uint64_t bootDoneTimeoutMs = 60000;
constexpr uint64_t bootDonePollMs = 1000;
constexpr std::array mstBurnFlags{"--override_cache_replacement",
                                  "--no_flash_verify", "--yes"};
constexpr std::array legacyBurnFlags{"--override_cache_replacement", "--yes"};
constexpr std::array holdBurnFlags{"--override_cache_replacement", "--yes",
                                   "--no_flash_verify", "--silent"};
constexpr std::array legacyProperties{
    "ChassisName", "ResetGPIO", "FlashNotPresentGPIO", "I2CBus", "I2CAddress"};
constexpr std::array holdProperties{"I2CBus",
                                    "I2CAddress",
                                    "ResetGPIO",
                                    "FlashNotPresentGPIO",
                                    "FlashNotPresentPolarity",
                                    "BootDoneGPIO"};
constexpr auto gpioStatusService = "xyz.openbmc_project.GpioStatusHandler";
constexpr auto gpioStatusPath = "/xyz/openbmc_project/GpioStatusHandler";
constexpr auto gpioStatusIface = "xyz.openbmc_project.GpioStatus";

std::atomic<int> pendingSignal{0};

struct Exit
{
    int rc;
};

std::string basename(const std::string& path)
{
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string hex8(uint32_t v)
{
    return std::format("0x{:08x}", v);
}

std::string seconds(uint64_t ms)
{
    if (ms % 1000 == 0)
    {
        return std::to_string(ms / 1000);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(ms) / 1000);
    return buf;
}

bool isDecimal(const std::string& s)
{
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

// i2ctransfer address parsing (base 0, 0x08..0x77 without -a).
std::optional<uint16_t> parseAddress(const std::string& text)
{
    if (text.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    auto v = std::strtol(text.c_str(), &end, 0);
    if (*end != '\0')
    {
        return std::nullopt;
    }
    if (v < 0x08 || v > 0x77)
    {
        return std::nullopt;
    }
    return static_cast<uint16_t>(v);
}

// printf '0x%02x' "$addr" as used by the livefish flow.
std::string hexAddress(const std::string& text)
{
    return std::format("0x{:02x}", std::strtoul(text.c_str(), nullptr, 0));
}

std::string replaceAll(std::string s, char from, char to)
{
    std::ranges::replace(s, from, to);
    return s;
}

// Value as busctl get-property prints it, e.g. `s "x"` or `b true`.
std::string busctlText(const Value& value)
{
    return std::visit(
        [&](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return "s \"" + x + "\"";
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return std::string("b ") + (x ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, uint8_t>)
            {
                return "y " + toText(value);
            }
            else if constexpr (std::is_same_v<T, int16_t>)
            {
                return "n " + toText(value);
            }
            else if constexpr (std::is_same_v<T, uint16_t>)
            {
                return "q " + toText(value);
            }
            else if constexpr (std::is_same_v<T, int32_t>)
            {
                return "i " + toText(value);
            }
            else if constexpr (std::is_same_v<T, uint32_t>)
            {
                return "u " + toText(value);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return "x " + toText(value);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return "t " + toText(value);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return "d " + toText(value);
            }
            else
            {
                return toText(value);
            }
        },
        value);
}

std::string propertyError(const std::string& prop, const std::string& iface,
                          const std::string& message)
{
    return std::format("Failed to get property {} on interface {}: {}", prop,
                       iface, message);
}

// tr '\r' '\n' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
std::string cleanCapture(std::string text)
{
    std::ranges::replace(text, '\r', '\n');
    static const std::regex ansi("\x1b\\[[0-9;]*[A-Za-z]");
    return std::regex_replace(text, ansi, "");
}

// tail -n 10 | tr '\n' ' '
std::string tailLines(const std::string& text, size_t count)
{
    if (text.empty())
    {
        return {};
    }
    size_t end = text.size();
    if (text.back() == '\n')
    {
        --end;
    }
    size_t start = 0;
    size_t seen = 0;
    for (size_t i = end; i > 0; --i)
    {
        if (text[i - 1] == '\n' && ++seen == count)
        {
            start = i;
            break;
        }
    }
    return replaceAll(text.substr(start), '\n', ' ');
}

} // namespace

std::expected<Policy, std::string> findPolicy(Platform& platform,
                                              const std::string& deviceType,
                                              unsigned attempts,
                                              uint64_t retryDelayMs)
{
    std::vector<std::pair<std::string, PropertyMap>> matches;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt)
    {
        matches.clear();
        auto paths =
            platform.getSubTreePaths(inventoryRoot, 0, {policyInterface});
        for (const auto& path : paths.value_or(std::vector<std::string>{}))
        {
            auto props = platform.getAll(emService, path, policyInterface);
            auto it =
                props ? props->find("DeviceType") : PropertyMap::iterator{};
            if (props && it != props->end() && toText(it->second) == deviceType)
            {
                matches.emplace_back(path, std::move(*props));
            }
        }
        if (!matches.empty())
        {
            break;
        }
        if (attempt < attempts)
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    if (matches.size() != 1)
    {
        return std::unexpected(
            std::format("{} MSTRecoveryPolicy for {}",
                        matches.empty() ? "no" : "more than one", deviceType));
    }
    auto policy = parsePolicy(matches[0].first, matches[0].second);
    if (!policy)
    {
        return std::unexpected("invalid MSTRecoveryPolicy for " + deviceType +
                               ": " + policy.error());
    }
    return policy;
}

Recovery::Recovery(Platform& platform, Policy policy, std::string program) :
    platform(platform), policy(std::move(policy)), program(std::move(program))
{}

int Recovery::run(std::vector<std::string> args)
{
    try
    {
        if (policy.flow == Flow::Hold)
        {
            return runHold(std::move(args));
        }
        return runGroups(std::move(args));
    }
    catch (const Exit& e)
    {
        return e.rc;
    }
    catch (const std::exception& e)
    {
        platform.writeErr(std::string("<3>mst-recovery: ") + e.what());
        cleanup();
        return 1;
    }
}

void Recovery::info(const std::string& obj, const std::string& msg)
{
    platform.writeOut(obj + ": " + msg);
}

void Recovery::error(const std::string& obj, const std::string& msg)
{
    if (policy.flow == Flow::Legacy)
    {
        platform.writeErr("<3>" + obj + ": Recovery failed: " + msg);
    }
    else
    {
        platform.writeErr("<3>" + obj + ": recovery failed: " + msg);
    }
}

int Recovery::runGroups(std::vector<std::string> args)
{
    bool force = false;
    if (!args.empty() && (args[0] == "-f" || args[0] == "--force"))
    {
        force = true;
        args.erase(args.begin());
    }
    if (args.size() != 1)
    {
        platform.writeErr(
            std::format("usage: {} [-f | --force] FW_IMAGE_PATH", program));
        return 1;
    }
    const auto& image = args[0];
    if (!platform.isReadableFile(image, false))
    {
        platform.writeErr(
            std::format("error: could not open `{}` for reading.", image));
        return 1;
    }
    if (policy.flow == Flow::Mst && !platform.commandExists("mstflint"))
    {
        platform.writeErr("<3>mstflint binary not found.");
        return 1;
    }

    auto start = platform.nowSeconds();
    size_t total = 0;
    std::vector<std::string> failed;
    size_t index = 0;
    for (const auto& group : policy.groups)
    {
        ++index;
        total += group.size();
        if (policy.flow == Flow::Mst && policy.groups.size() > 1)
        {
            std::string members;
            for (const auto& m : group)
            {
                members += (members.empty() ? "" : " ") + m;
            }
            info("recovery_main", std::format("starting group {}/{}: {}", index,
                                              policy.groups.size(), members));
        }

        std::vector<char> ok(group.size(), 0);
        std::vector<std::jthread> workers;
        for (size_t i = 0; i < group.size(); ++i)
        {
            workers.emplace_back(
                [&, i] { ok[i] = recoverDevice(group[i], image, force); });
        }
        workers.clear();
        for (size_t i = 0; i < group.size(); ++i)
        {
            if (ok[i] == 0)
            {
                failed.push_back(basename(group[i]));
            }
        }
    }

    if (policy.flow == Flow::Mst)
    {
        auto elapsed = platform.nowSeconds() - start;
        if (!failed.empty())
        {
            std::string list;
            for (const auto& f : failed)
            {
                list += (list.empty() ? "" : " ") + f;
            }
            error("recovery_main",
                  std::format("{} of {} device(s) failed in {}s: {}",
                              failed.size(), total, elapsed, list));
        }
        else
        {
            info("recovery_main",
                 std::format("all {} device(s) completed in {}s", total,
                             elapsed));
        }
    }
    return failed.empty() ? 0 : 1;
}

bool Recovery::recoverDevice(const std::string& relPath,
                             const std::string& image, bool force) noexcept
{
    try
    {
        return policy.flow == Flow::Legacy
                   ? performLegacy(relPath, image, force)
                   : performMst(relPath, image, force);
    }
    catch (const std::exception& e)
    {
        error(basename(relPath), e.what());
        return false;
    }
}

unsigned Recovery::attempts() const
{
    return policy.flow == Flow::Mst ? readAttempts : 1;
}

std::vector<std::string> Recovery::deviceProperties() const
{
    if (policy.flow == Flow::Legacy)
    {
        return {legacyProperties.begin(), legacyProperties.end()};
    }
    std::vector<std::string> props{"ResetGPIO", "I2CAddress",
                                   "FlashNotPresentGPIO", "I2CBus",
                                   "ChassisName"};
    if (policy.smaPresenceCheck)
    {
        props.insert(props.begin() + 2, "SMAEID");
    }
    return props;
}

std::optional<Recovery::Device> Recovery::readDevice(const std::string& relPath)
{
    Device dev;
    dev.path = policy.board + "/" + relPath;
    dev.name = basename(relPath);
    for (uint64_t attempt = 1; attempt <= attempts(); ++attempt)
    {
        dev.props.clear();
        bool complete = true;
        for (const auto& prop : deviceProperties())
        {
            auto value = platform.getProperty(emService, dev.path,
                                              policy.deviceInterface, prop);
            if (!value)
            {
                platform.writeErr(
                    propertyError(prop, policy.deviceInterface, value.error()));
                complete = false;
                break;
            }
            auto text = toText(*value);
            // printf %q of an empty value survives `local $defs` as ''.
            dev.props[prop] = text.empty() ? "''" : text;
        }
        if (complete)
        {
            if (policy.smaPresenceCheck)
            {
                auto name = platform.getProperty(
                    emService, dev.path, policy.deviceInterface, "SMAName");
                auto port = platform.getProperty(
                    emService, dev.path, policy.deviceInterface, "SMAUSBPort");
                if (name && port)
                {
                    dev.smaName = toText(*name);
                    dev.smaUsbPort = toText(*port);
                }
                else if (name)
                {
                    dev.smaName = toText(*name);
                }
            }
            return dev;
        }
        if (attempt < attempts())
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Recovery::readGis(const std::string& bus,
                                          const std::string& addr)
{
    auto parsed = parseAddress(addr);
    for (uint64_t attempt = 1; attempt <= attempts(); ++attempt)
    {
        if (parsed)
        {
            auto data = platform.i2cWriteRead(bus, *parsed, policy.gisRegister,
                                              policy.i2cForce);
            if (data)
            {
                return static_cast<uint32_t>((*data)[0]) << 24 |
                       static_cast<uint32_t>((*data)[1]) << 16 |
                       static_cast<uint32_t>((*data)[2]) << 8 |
                       static_cast<uint32_t>((*data)[3]);
            }
        }
        if (attempt < attempts())
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    return std::nullopt;
}

bool Recovery::gpioFindRetry(const std::string& name)
{
    for (uint64_t attempt = 1; attempt <= attempts(); ++attempt)
    {
        if (platform.gpioFind(name))
        {
            return true;
        }
        if (attempt < attempts())
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    return false;
}

bool Recovery::gpioSetRetry(const std::string& name, int value)
{
    for (uint64_t attempt = 1; attempt <= attempts(); ++attempt)
    {
        if (platform.gpioSet(name, value))
        {
            return true;
        }
        if (attempt < attempts())
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    return false;
}

bool Recovery::reset(const Device& dev, int fnpValue)
{
    const auto& rst = dev.props.at("ResetGPIO");
    const auto& fnp = dev.props.at("FlashNotPresentGPIO");
    if (policy.flow == Flow::Legacy)
    {
        // Best effort: each line is looked up when it is set
        for (auto [name, value] : {std::pair{&fnp, fnpValue}, {&rst, 0}})
        {
            if (gpioFindRetry(*name))
            {
                gpioSetRetry(*name, value);
            }
        }
        platform.sleepMs(resetPulseMs);
        if (gpioFindRetry(rst))
        {
            gpioSetRetry(rst, 1);
        }
        platform.sleepMs(resetSettleMs);
        return true;
    }
    if (!gpioFindRetry(fnp) || !gpioFindRetry(rst) ||
        !gpioSetRetry(fnp, fnpValue) || !gpioSetRetry(rst, 0))
    {
        return false;
    }
    platform.sleepMs(resetPulseMs);
    if (!gpioSetRetry(rst, 1))
    {
        return false;
    }
    platform.sleepMs(resetSettleMs);
    return true;
}

int Recovery::burn(const Device& dev, const std::string& image,
                   const std::string& addrText)
{
    std::vector<std::string> argv{"mstflint",
                                  "--device",
                                  "/dev/i2c-" + dev.props.at("I2CBus"),
                                  "--i2c_secondary",
                                  addrText,
                                  "--image",
                                  image};
    if (policy.flow == Flow::Legacy)
    {
        argv.insert(argv.end(), legacyBurnFlags.begin(), legacyBurnFlags.end());
        argv.emplace_back("burn");
        return platform.runProcess(argv, OutputMode::Inherit, {});
    }
    argv.insert(argv.end(), mstBurnFlags.begin(), mstBurnFlags.end());
    argv.emplace_back("burn");

    // awk RS="\r": every non-empty \r or \n separated piece is one line.
    std::string pending;
    auto emit = [&](std::string_view chunk) {
        for (char c : chunk)
        {
            if (c == '\r' || c == '\n')
            {
                if (!pending.empty())
                {
                    info(dev.name, pending);
                    pending.clear();
                }
            }
            else
            {
                pending += c;
            }
        }
    };
    auto rc = platform.runProcess(argv, OutputMode::Merged, emit);
    if (!pending.empty())
    {
        info(dev.name, pending);
    }
    return rc;
}

std::optional<std::string> Recovery::usbVidPid(const std::string& port)
{
    for (unsigned attempt = 1; attempt <= usbAttempts; ++attempt)
    {
        auto vid = platform.usbAttribute(port, "idVendor");
        auto pid =
            vid ? platform.usbAttribute(port, "idProduct") : std::nullopt;
        if (vid && pid && !vid->empty() && !pid->empty())
        {
            auto lower = [](std::string s) {
                std::ranges::transform(s, s.begin(), [](unsigned char c) {
                    return std::tolower(c);
                });
                return s;
            };
            return lower(*vid) + ":" + lower(*pid);
        }
        if (attempt < usbAttempts)
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    return std::nullopt;
}

std::optional<std::string> Recovery::mctpConnectivity(const std::string& eid)
{
    if (!isDecimal(eid))
    {
        return std::nullopt;
    }
    auto value = platform.getProperty(
        "au.com.codeconstruct.MCTP1",
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/" + eid,
        "au.com.codeconstruct.MCTP.Endpoint1", "Connectivity");
    if (!value)
    {
        return std::nullopt;
    }
    const auto* s = std::get_if<std::string>(&*value);
    if (s == nullptr)
    {
        return std::nullopt;
    }
    // cut -d '"' -f2
    auto pos = s->find('"');
    return pos == std::string::npos ? *s : s->substr(0, pos);
}

void Recovery::recoveryEvent(const std::string& msgId, const std::string& args,
                             const std::string& severity,
                             const std::string& resolution)
{
    if (policy.flow != Flow::Mst)
    {
        return;
    }
    std::map<std::string, std::string> data{{"REDFISH_MESSAGE_ID", msgId},
                                            {"REDFISH_MESSAGE_ARGS", args},
                                            {"namespace", "FWUpdate"}};
    if (!resolution.empty())
    {
        data[resolutionKey] = resolution;
    }
    platform.createLog(msgId, levelPrefix + severity, data);
}

void Recovery::resourceError(const std::string& obj, const std::string& message,
                             const std::string& resolution)
{
    recoveryEvent("ResourceEvent.1.0.ResourceErrorsDetected",
                  obj + "," + message, "Critical", resolution);
}

bool Recovery::handleMissingBus(const Device& dev, const std::string& vidPid)
{
    const auto& bus = dev.props.at("I2CBus");
    const auto& sma = dev.smaName;
    const auto& port = dev.smaUsbPort;
    if (port.empty())
    {
        error(dev.name,
              std::format("I2C bus /dev/i2c-{} not present and no SMA USB "
                          "port is mapped",
                          bus));
        resourceError(dev.name, "Device is not accessible",
                      deviceStateResolution);
        return false;
    }
    if (vidPid == mcuVidPidIsp)
    {
        info(dev.name, std::format("SMA {} is in ISP mode, skipping", sma));
        recoveryEvent("NvidiaUpdate.1.0.ComponentUpdateSkipped",
                      dev.name + ",its SMA MCU " + sma + " is in ISP mode");
        return true;
    }
    if (vidPid == mcuVidPidNormal)
    {
        error(dev.name,
              std::format("I2C bus /dev/i2c-{} not present but SMA {} is in "
                          "normal mode",
                          bus, sma));
        resourceError(dev.name,
                      "SMA MCU " + sma +
                          " is in normal mode but the device is not accessible",
                      deviceStateResolution);
        return false;
    }
    if (vidPid.empty())
    {
        error(dev.name,
              std::format("SMA {} is not present at USB port {}", sma, port));
        resourceError(dev.name,
                      "SMA MCU " + sma + " was not found at its USB port",
                      deviceStateResolution);
        return false;
    }
    error(dev.name, std::format("SMA {} at USB port {} reports unexpected {}",
                                sma, port, vidPid));
    resourceError(dev.name, "SMA MCU " + sma + " is not provisioned",
                  notProvisionedResolution);
    return false;
}

bool Recovery::performMst(const std::string& relPath, const std::string& image,
                          bool force)
{
    auto start = platform.nowSeconds();
    auto name = basename(relPath);
    auto completed = [&] {
        info(name,
             std::format("completed in {}s", platform.nowSeconds() - start));
    };

    auto read = readDevice(relPath);
    if (!read)
    {
        error(name, "failed to read D-Bus properties");
        return false;
    }
    const auto& dev = *read;
    const auto& bus = dev.props.at("I2CBus");
    const auto& addr = dev.props.at("I2CAddress");
    auto devPath = "/dev/i2c-" + bus;

    std::string vidPid;
    if (policy.smaPresenceCheck)
    {
        if (!dev.smaUsbPort.empty())
        {
            if (auto v = usbVidPid(dev.smaUsbPort))
            {
                vidPid = *v;
                info(name, std::format("SMA {} at USB port {} is {}",
                                       dev.smaName, dev.smaUsbPort, vidPid));
            }
            else
            {
                info(name, std::format("SMA {} is not present at USB port {}",
                                       dev.smaName, dev.smaUsbPort));
            }
        }
        else
        {
            const auto& eid = dev.props.at("SMAEID");
            if (auto c = mctpConnectivity(eid))
            {
                info(name,
                     std::format("SMA EID {} connectivity is {}", eid, *c));
            }
            else
            {
                info(name, std::format("SMA EID {} is not present", eid));
            }
        }
    }

    auto st = platform.statDevice(devPath);
    if (!st.isChar)
    {
        if (policy.smaPresenceCheck)
        {
            return handleMissingBus(dev, vidPid);
        }
        error(name, std::format("I2C bus /dev/i2c-{} not present", bus));
        return false;
    }
    if (!st.readable || !st.writable)
    {
        error(name,
              std::format("I2C device /dev/i2c-{} lacks read/write permissions",
                          bus));
        return false;
    }

    auto gis = readGis(bus, addr);
    if (!gis)
    {
        error(name, "could not retrieve boot status");
        return false;
    }
    info(name, policy.gisLabel + "=" + hex8(*gis));

    if (*gis == policy.gisHealthyValue)
    {
        if (force)
        {
            info(name, "booted successfully, forcing recovery");
        }
        else
        {
            info(name, "booted successfully, skipping recovery");
            if (policy.healthyEvent)
            {
                recoveryEvent("NvidiaUpdate.1.0.FirmwareNotInRecovery", name);
            }
            completed();
            return true;
        }
    }
    else
    {
        info(name, "boot failure, beginning recovery");
    }

    info(name, "asserting FNP and resetting");
    if (!reset(dev, 0))
    {
        error(name, "could not assert FNP and reset");
        return false;
    }
    recoveryEvent("NvidiaUpdate.1.0.RecoveryStarted", name);

    if (burn(dev, image, addr) != 0)
    {
        error(name, "burn failed, deasserting FNP");
        if (!reset(dev, 1))
        {
            error(name, "could not deassert FNP and reset after burn failure");
        }
        return false;
    }

    info(name, "burn complete, deasserting FNP");
    if (!reset(dev, 1))
    {
        error(name, "could not deassert FNP and reset after burn");
        return false;
    }

    for (uint64_t elapsedMs = 0;; elapsedMs += gisPollIntervalMs)
    {
        gis = readGis(bus, addr);
        if (!gis)
        {
            error(name, "could not retrieve boot status after burn");
            return false;
        }
        if (*gis == policy.gisHealthyValue)
        {
            break;
        }
        if (elapsedMs >= gisPollTimeoutMs)
        {
            error(name,
                  "boot status timeout after burn (GIS=" + hex8(*gis) + ")");
            return false;
        }
        info(name, std::format("waiting for boot (GIS={}, {}s/{}s)", hex8(*gis),
                               elapsedMs / 1000, gisPollTimeoutMs / 1000));
        platform.sleepMs(gisPollIntervalMs);
    }

    info(name, "recovered successfully");
    recoveryEvent("NvidiaUpdate.1.0.RecoverySuccessful", name);
    completed();
    return true;
}

bool Recovery::performLegacy(const std::string& relPath,
                             const std::string& image, bool force)
{
    auto name = basename(relPath);
    auto read = readDevice(relPath);
    if (!read)
    {
        platform.writeErr(
            std::format("<3>Failed to read D-Bus properties of `{}`.", name));
        return false;
    }
    const auto& dev = *read;
    const auto& chassis = dev.props.at("ChassisName");
    const auto& bus = dev.props.at("I2CBus");
    const auto& addr = dev.props.at("I2CAddress");

    auto st = platform.statDevice("/dev/i2c-" + bus);
    if (!st.isChar)
    {
        error(chassis, std::format("I2C bus /dev/i2c-{} not present.", bus));
        return false;
    }

    auto gis = readGis(bus, addr);
    if (!gis)
    {
        error(chassis, "Could not retrieve boot status.");
        return false;
    }
    info(chassis, policy.gisLabel + "=" + hex8(*gis));

    if (*gis == policy.gisHealthyValue)
    {
        if (!force)
        {
            info(chassis, "Booted successfully. Skipping recovery.");
            return true;
        }
        info(chassis, "Booted successfully. Forcing recovery.");
    }
    else
    {
        info(chassis, "Boot failure. Beginning recovery.");
    }

    info(chassis, "Asserting FNP and resetting.");
    reset(dev, 0);

    if (burn(dev, image, addr) != 0)
    {
        error(chassis, "Burn failed. Deasserting FNP.");
        reset(dev, 1);
        return false;
    }

    info(chassis, "Burn complete. Deasserting FNP.");
    reset(dev, 1);

    gis = readGis(bus, addr);
    if (!gis)
    {
        error(chassis, "Could not retrieve boot status.");
        return false;
    }
    if (*gis != policy.gisHealthyValue)
    {
        error(chassis, "Boot failure (GIS=" + hex8(*gis) + ").");
        return false;
    }
    info(chassis, "Recovered successfully.");
    return true;
}

// ---------------------------------------------------------------------------
// FNPMode HoldDeasserted (single device found through the mapper)

void Recovery::installSignalHandlers()
{
    struct sigaction sa{};
    sa.sa_handler = [](int sig) { pendingSignal = sig; };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

void Recovery::boundary()
{
    if (int sig = pendingSignal.exchange(0); sig != 0)
    {
        cleanup();
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }
}

void Recovery::tagged(const char* tag, const std::string& msg)
{
    boundary();
    platform.writeErr(std::format("[{}] {}", tag, msg));
}

void Recovery::cleanup()
{
    if (cleanedUp)
    {
        return;
    }
    cleanedUp = true;
    if (!assertedReset.empty())
    {
        platform.gpioSet(assertedReset, 1);
        assertedReset.clear();
    }
    if (holding)
    {
        platform.gpioRelease(fnpGpio);
        holding = false;
    }
    if (!succeeded && !fnpGpio.empty())
    {
        if (!platform.gpioFind(fnpGpio))
        {
            return;
        }
        platform.gpioSet(fnpGpio, fnpPolarity);
    }
}

void Recovery::finish(int rc)
{
    boundary();
    cleanup();
    throw Exit{rc};
}

void Recovery::logFirmwareUpdate(const std::string& level,
                                 const std::string& message,
                                 const std::string& msgId,
                                 const std::string& args, bool withNamespace)
{
    boundary();
    std::map<std::string, std::string> data{
        {"DEVICE", deviceName.empty() ? "unknown" : deviceName},
        {"REDFISH_MESSAGE_ID", msgId},
        {"REDFISH_MESSAGE_ARGS", args}};
    if (withNamespace)
    {
        data["namespace"] = "FWUpdate";
    }
    platform.createLog(message, levelPrefix + level, data);
}

void Recovery::die(int rc, const std::string& msg)
{
    tagged("ERROR", msg);
    auto dev = deviceName.empty() ? std::string("unknown") : deviceName;
    auto ver = imageVersion.empty() ? std::string("unknown") : imageVersion;
    auto sanitize = [](std::string s) {
        return replaceAll(replaceAll(std::move(s), ',', ';'), '=', ':');
    };
    auto message = policy.deviceType + " livefish recovery failed: " + msg;
    logFirmwareUpdate("Error", message, "OpenBMC.0.1.FirmwareUpdateFailed",
                      dev + "," + ver + "," + sanitize(msg), false);
    auto reason = msg;
    if (auto pos = reason.find("-E- "); pos != std::string::npos)
    {
        reason = reason.substr(pos + 4);
    }
    logFirmwareUpdate("Error", message,
                      "ResourceEvent.1.0.ResourceErrorsDetected",
                      dev + " recovery," + sanitize(reason), true);
    finish(rc);
}

int Recovery::runHold(std::vector<std::string> args)
{
    constexpr int failure = 1;
    constexpr int invalidArgument = 2;
    constexpr int notConfigured = 6;
    auto info = [&](const std::string& msg) { tagged("INFO", msg); };
    auto warn = [&](const std::string& msg) { tagged("WARN", msg); };

    if (args.size() != 1)
    {
        die(invalidArgument, "Usage: " + program + " <image>");
    }
    const auto image = args[0];
    if (!platform.isReadableFile(image, true))
    {
        die(invalidArgument, "Image not found or unreadable: " + image);
    }

    const auto& iface = policy.deviceInterface;
    auto ifaceShort = iface.substr(iface.find_last_of('.') + 1);
    std::vector<std::string> paths;
    for (uint64_t attempt = 1; attempt <= readAttempts; ++attempt)
    {
        boundary();
        auto found = platform.getSubTreePaths(inventoryRoot, 0, {iface});
        if (found && !found->empty())
        {
            paths = std::move(*found);
            break;
        }
        if (attempt < readAttempts)
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    if (paths.empty())
    {
        die(notConfigured,
            std::format("No {} object found under {}", iface, inventoryRoot));
    }
    const auto board = basename(policy.board);
    const auto filter = "/" + board + "/";
    std::vector<std::string> matched;
    std::string all;
    for (const auto& p : paths)
    {
        all += p + " ";
        if (p.find(filter) != std::string::npos)
        {
            matched.push_back(p);
        }
    }
    if (matched.empty())
    {
        die(notConfigured,
            std::format("No {} object under board {} (paths: {})", iface, board,
                        all));
    }
    if (matched.size() > 1)
    {
        warn(std::format("Found {} {} objects under {}, using the first",
                         matched.size(), ifaceShort, board));
    }
    const auto emPath = matched.front();
    deviceName = basename(emPath);

    boundary();
    auto version = platform.getProperty(
        std::format("com.Nvidia.Software.{}Recovery.Updater",
                    policy.deviceType),
        std::format("/xyz/openbmc_project/software/other/{}Recovery",
                    policy.deviceType),
        "xyz.openbmc_project.Software.ExtendedVersion", "ExtendedVersion");
    if (version)
    {
        if (const auto* s = std::get_if<std::string>(&*version))
        {
            imageVersion = *s;
        }
    }
    if (imageVersion.empty())
    {
        imageVersion = "unknown";
    }

    std::vector<std::string> values;
    for (uint64_t attempt = 1; attempt <= readAttempts; ++attempt)
    {
        boundary();
        values.clear();
        for (const auto* prop : holdProperties)
        {
            auto v = platform.getProperty(emService, emPath, iface, prop);
            if (!v)
            {
                values.clear();
                break;
            }
            values.push_back(toText(*v));
        }
        if (!values.empty())
        {
            break;
        }
        if (attempt < readAttempts)
        {
            platform.sleepMs(retryDelayMs);
        }
    }
    if (values.empty())
    {
        std::string props;
        for (const auto* p : holdProperties)
        {
            props += std::string(props.empty() ? "" : " ") + p;
        }
        die(notConfigured, std::format("Failed to read {} from {} {}", props,
                                       emService, emPath));
    }
    std::map<std::string, std::string> v;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (values[i].empty())
        {
            die(notConfigured, std::format("EM property {} is empty on {}",
                                           holdProperties[i], emPath));
        }
        v[holdProperties[i]] = values[i];
    }

    const auto bus = v["I2CBus"];
    const auto addr = v["I2CAddress"];
    const auto polarity = v["FlashNotPresentPolarity"];
    if (!isDecimal(bus))
    {
        die(notConfigured, std::format("Invalid I2CBus '{}' (expected a "
                                       "decimal bus number)",
                                       bus));
    }
    if (!isDecimal(addr))
    {
        die(notConfigured, std::format("Invalid I2CAddress '{}' (expected a "
                                       "decimal address)",
                                       addr));
    }
    if (polarity != "0" && polarity != "1")
    {
        die(notConfigured,
            std::format("Invalid FlashNotPresentPolarity '{}' (expected 0 or "
                        "1)",
                        polarity));
    }
    const auto addrHex = hexAddress(addr);
    const auto resetGpio = v["ResetGPIO"];
    const auto bootDone = v["BootDoneGPIO"];
    fnpGpio = v["FlashNotPresentGPIO"];
    fnpPolarity = polarity == "1" ? 1 : 0;
    const int fnpDeassert = 1 - fnpPolarity;
    const auto devPath = "/dev/i2c-" + bus;

    boundary();
    if (!platform.statDevice(devPath).exists)
    {
        die(invalidArgument, "I2C bus " + devPath + " not present");
    }

    auto size = platform.fileSize(image);
    info("Config : " + emPath);
    info("I2C    : bus=" + bus + " addr=" + addrHex);
    info("Reset  : gpio=" + resetGpio + " pulse=" + seconds(resetPulseMs) +
         "s");
    info("GPIO   : fnp=" + fnpGpio +
         " fnp_polarity=" + std::to_string(fnpPolarity));
    info(std::format("Boot   : status={}.{}", gpioStatusService, bootDone));
    info("Device : " + deviceName + " version=" + imageVersion);
    info(std::format("Image  : {} ({} bytes)", image,
                     size ? std::to_string(*size) : std::string("?")));

    auto parsedAddr = parseAddress(addrHex);
    auto readOnce = [&]() -> std::optional<uint32_t> {
        boundary();
        if (!parsedAddr)
        {
            return std::nullopt;
        }
        auto d = platform.i2cWriteRead(bus, *parsedAddr, policy.gisRegister,
                                       policy.i2cForce);
        if (!d)
        {
            return std::nullopt;
        }
        return static_cast<uint32_t>((*d)[0]) << 24 |
               static_cast<uint32_t>((*d)[1]) << 16 |
               static_cast<uint32_t>((*d)[2]) << 8 |
               static_cast<uint32_t>((*d)[3]);
    };

    if (auto gis = readOnce(); !gis)
    {
        info("Pre-burn GIS read failed - proceeding with recovery");
    }
    else
    {
        info("Pre-burn GIS=" + hex8(*gis));
        if (*gis == policy.gisHealthyValue)
        {
            info("NIC already booted successfully - skipping recovery");
            succeeded = true;
            logFirmwareUpdate(
                "Informational",
                std::format("Firmware '{}' is not in Recovery.", deviceName),
                "NvidiaUpdate.1.0.FirmwareNotInRecovery", deviceName, true);
            finish(0);
        }
    }

    boundary();
    if (!platform.gpioFind(fnpGpio))
    {
        die(notConfigured, "GPIO '" + fnpGpio + "' not found");
    }
    boundary();
    bool held = platform.gpioHold(fnpGpio, fnpDeassert);
    holding = held;
    platform.sleepMs(holdSettleMs);
    if (!held)
    {
        die(notConfigured, std::format("Failed to hold GPIO '{}' at value '{}'",
                                       fnpGpio, fnpDeassert));
    }
    info("FNP deasserted - NIC can access SPI flash");

    std::vector<std::string> argv{
        "mstflint", "--device", devPath, "--i2c_secondary",
        addrHex,    "--image",  image};
    argv.insert(argv.end(), holdBurnFlags.begin(), holdBurnFlags.end());
    argv.emplace_back("burn");
    std::string output;
    uint64_t attempt = 1;
    bool burned = false;
    for (; attempt <= burnAttempts; ++attempt)
    {
        info(std::format("Burn attempt {}/{}...", attempt, burnAttempts));
        output.clear();
        boundary();
        int rc = platform.runProcess(
            argv, OutputMode::Merged,
            [&](std::string_view chunk) { output.append(chunk); });
        output = cleanCapture(output);
        if (rc == 0)
        {
            info("Burn succeeded");
            burned = true;
            break;
        }
        if (output.find(burnRetryPattern) != std::string::npos &&
            attempt < burnAttempts)
        {
            warn(std::format("Device not ready, retrying in {}s...",
                             seconds(burnRetryDelayMs)));
            platform.sleepMs(burnRetryDelayMs);
        }
        else
        {
            break;
        }
    }
    if (!burned)
    {
        die(failure, std::format("Burn failed after {} attempt(s): {}",
                                 std::min(attempt, burnAttempts),
                                 tailLines(output, 10)));
    }

    boundary();
    if (!platform.gpioFind(resetGpio))
    {
        die(notConfigured, "Reset GPIO " + resetGpio + " not found");
    }
    boundary();
    if (!platform.gpioSet(resetGpio, 0))
    {
        die(notConfigured, "Failed to assert " + resetGpio + " low");
    }
    assertedReset = resetGpio;
    platform.sleepMs(resetPulseMs);
    boundary();
    if (!platform.gpioSet(resetGpio, 1))
    {
        die(notConfigured, "Failed to deassert " + resetGpio + " high");
    }
    assertedReset.clear();
    info("NIC reset pulsed via GPIO " + resetGpio);

    info(std::format("Waiting {}s for NIC to settle after reset...",
                     seconds(bootDoneSettleMs)));
    platform.sleepMs(bootDoneSettleMs);
    info(std::format("Polling {} via {} (up to {}s, active-low)...", bootDone,
                     gpioStatusService, seconds(bootDoneTimeoutMs)));
    bool done = false;
    for (uint64_t elapsed = 0; elapsed < bootDoneTimeoutMs;
         elapsed += bootDonePollMs)
    {
        boundary();
        auto val = platform.getProperty(gpioStatusService, gpioStatusPath,
                                        gpioStatusIface, bootDone);
        if (!val)
        {
            die(notConfigured,
                std::format(
                    "Failed to read {} from {}: {}", bootDone,
                    gpioStatusService,
                    propertyError(bootDone, gpioStatusIface, val.error())));
        }
        const auto* b = std::get_if<bool>(&*val);
        if (b == nullptr)
        {
            die(notConfigured, std::format("Unexpected {} D-Bus value '{}'",
                                           bootDone, busctlText(*val)));
        }
        if (!*b)
        {
            info(std::format("NIC boot done after {}s", seconds(elapsed)));
            done = true;
            break;
        }
        platform.sleepMs(bootDonePollMs);
    }
    if (!done)
    {
        die(failure, std::format("NIC boot-done not detected within {}s",
                                 seconds(bootDoneTimeoutMs)));
    }

    auto gis = readOnce();
    if (!gis)
    {
        die(failure, "Post-burn GIS read failed - NIC not responding on I2C");
    }
    info("Post-burn GIS=" + hex8(*gis));
    if (*gis != policy.gisHealthyValue)
    {
        die(failure,
            std::format("NIC did not boot healthy (GIS={}, expected {})",
                        hex8(*gis), hex8(policy.gisHealthyValue)));
    }
    info("Post-burn verification succeeded");

    succeeded = true;
    auto message =
        policy.deviceType + " livefish recovery completed successfully";
    auto okArgs = deviceName + "," + imageVersion;
    logFirmwareUpdate("Informational", message,
                      "OpenBMC.0.1.FirmwareUpdateCompleted", okArgs, false);
    logFirmwareUpdate("Informational", message,
                      "OpenBMC.0.1.FirmwareUpdateCompleted", okArgs, true);
    info(policy.deviceType + " livefish recovery completed");
    finish(0);
}

} // namespace mst
