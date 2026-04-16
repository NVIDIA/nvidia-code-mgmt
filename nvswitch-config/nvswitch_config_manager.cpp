/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "nvswitch_config_manager.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace nvidia::nvswitch::config
{

namespace fs = std::filesystem;
namespace match = sdbusplus::bus::match;

// ---------------------------------------------------------------------------
// BlobSendContext – carries all state for one async config-apply chain.
// Lifetime: from applyConfigToDevice() until finishApply() runs.
// ---------------------------------------------------------------------------

struct NVSwitchConfigManager::BlobSendContext
{
    std::string devicePath;
    std::string serviceName; // resolved via ObjectMapper.GetObject

    struct BlobEntry
    {
        std::vector<uint8_t> data;
        std::string phase; // "ports_down" | "configure" | "ports_up"
        size_t idxInPhase{0};
    };

    std::vector<BlobEntry>
        blobs; // flattened: ports_down → configure → ports_up
    size_t current{0};
    uint64_t successCount{0};
    uint64_t failCount{0};
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

NVSwitchConfigManager::NVSwitchConfigManager(
    boost::asio::io_context& ioc,
    std::shared_ptr<sdbusplus::asio::connection> conn) :
    ioc_(ioc), conn_(std::move(conn)), server_(conn_)
{
    iface_ = server_.add_interface(managerObjectPath, managerInterface);
    iface_->register_method("AddConfigFile",
                            [this](const sdbusplus::message::unix_fd& fileFd) {
                                handleAddConfigFile(fileFd);
                            });
    iface_->register_method("RemoveConfigFile",
                            [this]() { handleRemoveConfigFile(); });
    iface_->initialize();

    lg2::info(
        "NVSwitchConfigManager: dbus interface registered at {PATH} on {IFACE}",
        "PATH", managerObjectPath, "IFACE", managerInterface);

    setupSignalWatchers();
}

// ---------------------------------------------------------------------------
// Phase 1 – dbus method handlers
// ---------------------------------------------------------------------------

void NVSwitchConfigManager::handleAddConfigFile(
    const sdbusplus::message::unix_fd& fileFd)
{
    lg2::info("handleAddConfigFile: with fd={FD}", "FD",
              static_cast<int>(fileFd.fd));

    auto configFilePath = getConfigFilePath();

    // All D-Bus method calls run on the io_context thread; no interleaving
    // is possible, so no mutex is needed for these file-existence checks.
    if (fs::exists(configFilePath))
    {
        lg2::error("AddConfigFile: file already exists at {PATH}", "PATH",
                   configFilePath.string());
        throw std::runtime_error(
            "FileAlreadyExists: Config file already exists. "
            "Invoke RemoveConfigFile before adding a new one.");
    }

    std::string content;
    try
    {
        content = readFromFd(fileFd.fd);
    }
    catch (const std::exception& e)
    {
        lg2::error("AddConfigFile: failed to read from fd: {ERR}", "ERR",
                   e.what());
        throw;
    }

    lg2::info("handleAddConfigFile: read {SZ} bytes from fd", "SZ",
              static_cast<uint64_t>(content.size()));

    if (content.empty())
        throw std::runtime_error("Config file content is empty.");

    if (content.size() > maxConfigFileSize)
    {
        lg2::error("AddConfigFile: content {SZ} bytes exceeds limit {MAX}",
                   "SZ", static_cast<uint64_t>(content.size()), "MAX",
                   static_cast<uint64_t>(maxConfigFileSize));
        throw std::runtime_error("Config file exceeds maximum allowed size.");
    }

    try
    {
        parseConfigFile(content);
    }
    catch (const std::exception& e)
    {
        lg2::error("AddConfigFile: config validation failed: {ERR}", "ERR",
                   e.what());
        throw std::runtime_error(
            std::string("Config file validation failed: ") + e.what());
    }

    std::error_code ec;
    fs::create_directories(configFilePath.parent_path(), ec);
    if (ec)
    {
        lg2::error("AddConfigFile: failed to create directory {DIR}: {ERR}",
                   "DIR", configFilePath.parent_path().string(), "ERR",
                   ec.message());
        throw std::runtime_error(
            std::string("Failed to create config directory: ") + ec.message());
    }

    auto tempPath = configFilePath.string() + ".tmp";
    {
        std::ofstream ofs(tempPath,
                          std::ios::out | std::ios::binary | std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("Failed to open temp file for writing.");

        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs)
        {
            ofs.close();
            fs::remove(tempPath, ec);
            throw std::runtime_error("Failed to write config content to disk.");
        }
    }

    fs::rename(tempPath, configFilePath, ec);
    if (ec)
    {
        fs::remove(tempPath);
        throw std::runtime_error(std::string("Failed to store config file: ") +
                                 ec.message());
    }
    lg2::info("AddConfigFile: complete – config persisted at {PATH}", "PATH",
              configFilePath.string());
}

void NVSwitchConfigManager::handleRemoveConfigFile()
{
    auto configFilePath = getConfigFilePath();
    if (!fs::exists(configFilePath))
    {
        lg2::warning("RemoveConfigFile: no config file found at {PATH}", "PATH",
                     configFilePath.string());
        throw std::runtime_error("No config file present at " +
                                 configFilePath.string());
    }

    std::error_code ec;
    fs::remove(configFilePath, ec);
    if (ec)
    {
        lg2::error("RemoveConfigFile: failed to remove {PATH}: {ERR}", "PATH",
                   configFilePath.string(), "ERR", ec.message());
        throw std::runtime_error(std::string("Failed to remove config file: ") +
                                 ec.message());
    }
    lg2::info("RemoveConfigFile: removed config file at {PATH}", "PATH",
              configFilePath.string());
}

// ---------------------------------------------------------------------------
// Phase 2 – signal setup and dispatch
// ---------------------------------------------------------------------------

void NVSwitchConfigManager::setupSignalWatchers()
{
    // Register the InterfacesAdded watcher BEFORE the initial GetSubTree query
    // so that devices appearing in the window between the two are not missed.
    const std::string ifacesAddedRule =
        match::rules::type::signal() +
        match::rules::interface("org.freedesktop.DBus.ObjectManager") +
        match::rules::member("InterfacesAdded");

    signalMatches_.push_back(std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus::bus&>(*conn_), ifacesAddedRule,
        [this](sdbusplus::message::message& msg) { onInterfacesAdded(msg); }));

    // Kick off the initial async device discovery; will execute once
    // io_context.run() starts.
    discoverAndSubscribeDevices();
}

void NVSwitchConfigManager::discoverAndSubscribeDevices()
{
    lg2::info("discoverAndSubscribeDevices: querying ObjectMapper GetSubTree "
              "for iface={IFACE}",
              "IFACE", requestConfigIface);

    conn_->async_method_call(
        [this](boost::system::error_code ec,
               const std::map<std::string,
                              std::map<std::string, std::vector<std::string>>>&
                   subtree) {
            if (ec)
            {
                lg2::error(
                    "discoverAndSubscribeDevices: GetSubTree failed: {ERR} "
                    "– will rely on InterfacesAdded for future devices",
                    "ERR", ec.message());
                return;
            }

            for (const auto& [objPath, serviceMap] : subtree)
            {
                lg2::debug("discoverAndSubscribeDevices: found path={PATH}",
                           "PATH", objPath);
                subscribeToDevice(objPath);
            }
        },
        objectMapperBusName, objectMapperPath, objectMapperIface, "GetSubTree",
        "/xyz/openbmc_project/inventory", 0,
        std::vector<std::string>{requestConfigIface});
}

void NVSwitchConfigManager::subscribeToDevice(const std::string& devicePath)
{
    if (deviceStates_.count(devicePath))
    {
        lg2::debug("subscribeToDevice: {PATH} already subscribed – skipping",
                   "PATH", devicePath);
        return;
    }

    deviceStates_.emplace(devicePath, std::make_shared<DeviceState>());
    const std::string matchRule = match::rules::type::signal() +
                                  match::rules::interface(requestConfigIface) +
                                  match::rules::member(configUpdateSignal) +
                                  match::rules::path(devicePath);

    signalMatches_.push_back(std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus::bus&>(*conn_), matchRule,
        [this, devicePath](sdbusplus::message::message& /*msg*/) {
            onConfigUpdateSignal(devicePath);
        }));

    lg2::info("NVSwitchConfigManager: subscribed to {SIG} on {PATH}", "SIG",
              configUpdateSignal, "PATH", devicePath);
}

void NVSwitchConfigManager::onInterfacesAdded(sdbusplus::message::message& msg)
{
    using PropVariant = std::variant<bool, uint8_t, int16_t, uint16_t, int32_t,
                                     uint32_t, int64_t, uint64_t, double,
                                     std::string, std::vector<std::string>>;
    using IfaceMap = std::map<std::string, std::map<std::string, PropVariant>>;

    sdbusplus::message::object_path objPath;
    IfaceMap interfaces;
    try
    {
        msg.read(objPath, interfaces);
    }
    catch (const std::exception& e)
    {
        lg2::error("onInterfacesAdded: failed to parse message: {ERR}", "ERR",
                   e.what());
        return;
    }
    if (interfaces.find(requestConfigIface) == interfaces.end())
    {
        return;
    }

    const std::string addedPath = std::string(objPath);
    lg2::info(
        "onInterfacesAdded: new device {PATH} exposes {IFACE} – subscribing",
        "PATH", addedPath, "IFACE", requestConfigIface);

    subscribeToDevice(addedPath);
}

void NVSwitchConfigManager::onConfigUpdateSignal(const std::string& devicePath)
{
    lg2::info("DeviceConfigurationRequested signal received from {PATH}",
              "PATH", devicePath);

    auto it = deviceStates_.find(devicePath);
    if (it == deviceStates_.end())
    {
        lg2::error(
            "onConfigUpdateSignal: unexpected path {PATH} – no state entry",
            "PATH", devicePath);
        return;
    }

    auto& state = it->second;
    if (state->inProgress)
    {
        lg2::warning("Config application already in progress for {PATH}, "
                     "ignoring duplicate signal",
                     "PATH", devicePath);
        return;
    }

    state->inProgress = true;
    applyConfigToDevice(devicePath);
}

// ---------------------------------------------------------------------------
// Async config-application chain
// ---------------------------------------------------------------------------

void NVSwitchConfigManager::applyConfigToDevice(const std::string& devicePath)
{
    auto configFilePath = getConfigFilePath();

    if (!fs::exists(configFilePath))
    {
        lg2::warning("applyConfigToDevice: no config file present for {PATH}; "
                     "skipping",
                     "PATH", devicePath);
        deviceStates_[devicePath]->inProgress = false;
        return;
    }

    std::string content;
    {
        std::ifstream ifs(configFilePath, std::ios::binary);
        if (!ifs)
        {
            lg2::error(
                "applyConfigToDevice: cannot open config file for {PATH}",
                "PATH", devicePath);
            deviceStates_[devicePath]->inProgress = false;
            return;
        }
        content.assign(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
    }

    if (content.empty())
    {
        lg2::error("applyConfigToDevice: config file is empty for {PATH}",
                   "PATH", devicePath);
        deviceStates_[devicePath]->inProgress = false;
        return;
    }

    SwitchConfig config;
    try
    {
        config = parseConfigFile(content);
    }
    catch (const std::exception& e)
    {
        lg2::error("applyConfigToDevice: parse failed for {PATH}: {ERR}",
                   "PATH", devicePath, "ERR", e.what());
        deviceStates_[devicePath]->inProgress = false;
        return;
    }

    // ---- Build flat blob list: ports_down → configure → ports_up ----
    auto ctx = std::make_shared<BlobSendContext>();
    ctx->devicePath = devicePath;

    for (size_t i = 0; i < config.portsDown.size(); ++i)
        ctx->blobs.push_back({config.portsDown[i], "ports_down", i});
    for (size_t i = 0; i < config.configure.size(); ++i)
        ctx->blobs.push_back({config.configure[i], "configure", i});
    for (size_t i = 0; i < config.portsUp.size(); ++i)
        ctx->blobs.push_back({config.portsUp[i], "ports_up", i});

    lg2::info("applyConfigToDevice: {PATH} – sending {TOTAL} blobs "
              "({PD} ports_down, {C} configure, {PU} ports_up)",
              "PATH", devicePath, "TOTAL",
              static_cast<uint64_t>(ctx->blobs.size()), "PD",
              static_cast<uint64_t>(config.portsDown.size()), "C",
              static_cast<uint64_t>(config.configure.size()), "PU",
              static_cast<uint64_t>(config.portsUp.size()));

    // ---- Async: resolve service that owns deviceConfigIface ----
    conn_->async_method_call(
        [this,
         ctx](boost::system::error_code ec,
              const std::map<std::string, std::vector<std::string>>& services) {
            if (ec || services.empty())
            {
                lg2::error("applyConfigToDevice: cannot resolve service for "
                           "{PATH}: {ERR}",
                           "PATH", ctx->devicePath, "ERR", ec.message());
                finishApply(ctx);
                return;
            }
            ctx->serviceName = services.begin()->first;
            sendNextBlob(ctx);
        },
        objectMapperBusName, objectMapperPath, objectMapperIface, "GetObject",
        devicePath, std::vector<std::string>{deviceConfigIface});
}

void NVSwitchConfigManager::sendNextBlob(std::shared_ptr<BlobSendContext> ctx)
{
    if (ctx->current >= ctx->blobs.size())
    {
        finishApply(ctx);
        return;
    }

    const auto& entry = ctx->blobs[ctx->current];
    lg2::debug(
        "sendNextBlob: sending {PHASE}[{IDX}] sz={SZ} to {PATH}", "PHASE",
        entry.phase, "IDX", static_cast<uint64_t>(entry.idxInPhase), "SZ",
        static_cast<uint64_t>(entry.data.size()), "PATH", ctx->devicePath);

    {
        std::string hexDump;
        hexDump.reserve(entry.data.size() * 2);
        static constexpr char hexChars[] = "0123456789abcdef";
        for (uint8_t byte : entry.data)
        {
            hexDump.push_back(hexChars[(byte >> 4) & 0xf]);
            hexDump.push_back(hexChars[byte & 0xf]);
        }
        lg2::debug("[FLOW] sendNextBlob: blob content {PHASE}[{IDX}] = {HEX}",
                   "PHASE", entry.phase, "IDX",
                   static_cast<uint64_t>(entry.idxInPhase), "HEX", hexDump);
    }

    // Create memfd and populate it synchronously (fast, in-kernel operation)
    int fd = -1;
    try
    {
        fd = createMemfd(entry.data);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "sendNextBlob: memfd failed for {PHASE}[{IDX}] on {PATH}: {ERR}",
            "PHASE", entry.phase, "IDX",
            static_cast<uint64_t>(entry.idxInPhase), "PATH", ctx->devicePath,
            "ERR", e.what());
        ++ctx->failCount;
        ++ctx->current;
        sendNextBlob(ctx);
        return;
    }

    // async_method_call constructs the D-Bus message (duping the fd) before
    // returning, so the original fd can be closed immediately afterwards.
    conn_->async_method_call(
        [this, ctx](boost::system::error_code ec,
                    sdbusplus::message::object_path asyncPath) {
            const auto& e = ctx->blobs[ctx->current];
            if (ec)
            {
                lg2::error("sendNextBlob: SetDeviceConfiguration failed for "
                           "{PHASE}[{IDX}] on {PATH}: {ERR}",
                           "PHASE", e.phase, "IDX",
                           static_cast<uint64_t>(e.idxInPhase), "PATH",
                           ctx->devicePath, "ERR", ec.message());
                ++ctx->failCount;
                ++ctx->current;
                sendNextBlob(ctx);
                return;
            }
            lg2::debug("sendNextBlob: {PHASE}[{IDX}] call returned – "
                       "async status path={ASYNC}",
                       "PHASE", e.phase, "IDX",
                       static_cast<uint64_t>(e.idxInPhase), "ASYNC",
                       std::string(asyncPath));
            pollBlobStatus(ctx, std::string(asyncPath), 0);
        },
        ctx->serviceName, ctx->devicePath, deviceConfigIface,
        setDeviceConfigMethod, std::string(nvlinkAccessConfigType),
        sdbusplus::message::unix_fd(fd));

    // sd-bus duped the fd during message construction above; close original.
    ::close(fd);
}

void NVSwitchConfigManager::pollBlobStatus(std::shared_ptr<BlobSendContext> ctx,
                                           std::string asyncPath,
                                           unsigned int elapsedMs)
{
    static const std::string inProgressValue =
        "com.nvidia.Async.Status.AsyncOperationStatus.InProgress";
    static const std::string successValue =
        "com.nvidia.Async.Status.AsyncOperationStatus.Success";
    static constexpr unsigned int pollIntervalMs = 200;
    static constexpr unsigned int timeoutMs = 30000;

    conn_->async_method_call(
        [this, ctx, asyncPath,
         elapsedMs](boost::system::error_code ec,
                    const std::variant<std::string>& prop) {
            const auto& e = ctx->blobs[ctx->current];

            if (ec)
            {
                lg2::error(
                    "pollBlobStatus: failed to read Status at {ASYNC}: {ERR}",
                    "ASYNC", asyncPath, "ERR", ec.message());
                ++ctx->failCount;
                ++ctx->current;
                sendNextBlob(ctx);
                return;
            }

            const auto& status = std::get<std::string>(prop);
            if (status == inProgressValue)
            {
                if (elapsedMs >= timeoutMs)
                {
                    lg2::error(
                        "pollBlobStatus: timeout waiting for {PHASE}[{IDX}] "
                        "on {PATH}",
                        "PHASE", e.phase, "IDX",
                        static_cast<uint64_t>(e.idxInPhase), "PATH",
                        ctx->devicePath);
                    ++ctx->failCount;
                    ++ctx->current;
                    sendNextBlob(ctx);
                    return;
                }
                // Schedule the next poll using a steady_timer – does not
                // block the event loop.
                auto timer = std::make_shared<boost::asio::steady_timer>(ioc_);
                timer->expires_after(std::chrono::milliseconds(pollIntervalMs));
                timer->async_wait([this, ctx, asyncPath, elapsedMs,
                                   timer](boost::system::error_code tec) {
                    if (!tec)
                        pollBlobStatus(ctx, asyncPath,
                                       elapsedMs + pollIntervalMs);
                    // If tec is set the timer was cancelled; do nothing.
                });
                return;
            }

            // Operation finished (success or failure)
            bool ok = (status == successValue);
            if (ok)
            {
                ++ctx->successCount;
                lg2::info(
                    "applyConfigToDevice: {PHASE}[{IDX}] success on {PATH}",
                    "PHASE", e.phase, "IDX",
                    static_cast<uint64_t>(e.idxInPhase), "PATH",
                    ctx->devicePath);
            }
            else
            {
                ++ctx->failCount;
                lg2::error(
                    "applyConfigToDevice: {PHASE}[{IDX}] FAILED on {PATH} "
                    "status={ST}",
                    "PHASE", e.phase, "IDX",
                    static_cast<uint64_t>(e.idxInPhase), "PATH",
                    ctx->devicePath, "ST", status);
            }
            ++ctx->current;
            sendNextBlob(ctx);
        },
        ctx->serviceName, asyncPath, "org.freedesktop.DBus.Properties", "Get",
        asyncStatusIface, std::string("Status"));
}

void NVSwitchConfigManager::finishApply(std::shared_ptr<BlobSendContext> ctx)
{
    uint64_t total = static_cast<uint64_t>(ctx->blobs.size());
    lg2::info("applyConfigToDevice: COMPLETE for {PATH} – "
              "{SUCCESS}/{TOTAL} blobs OK, {FAIL} failure(s)",
              "PATH", ctx->devicePath, "SUCCESS", ctx->successCount, "TOTAL",
              total, "FAIL", ctx->failCount);

    auto it = deviceStates_.find(ctx->devicePath);
    if (it != deviceStates_.end())
        it->second->inProgress = false;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::filesystem::path NVSwitchConfigManager::getConfigFilePath()
{
    return std::filesystem::path(configDir) / configFileName;
}

std::string NVSwitchConfigManager::readFromFd(int fd)
{
    if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1))
    {
        lg2::debug("readFromFd: lseek failed (fd may not be seekable): {ERR}",
                   "ERR", std::strerror(errno));
    }

    std::string content;
    content.reserve(4096);

    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
    {
        if (content.size() + static_cast<std::size_t>(n) > maxConfigFileSize)
        {
            throw std::runtime_error(
                "Config file content exceeds maximum allowed size (" +
                std::to_string(maxConfigFileSize) + " bytes).");
        }
        content.append(buf, static_cast<std::size_t>(n));
    }

    if (n < 0)
    {
        throw std::system_error(errno, std::generic_category(),
                                "Failed to read from file descriptor");
    }
    return content;
}

void NVSwitchConfigManager::trimInPlace(std::string& s)
{
    auto rend = std::find_if_not(
        s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); });
    s.erase(rend.base(), s.end());

    auto fwd = std::find_if_not(
        s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    s.erase(s.begin(), fwd);
}

std::vector<uint8_t>
    NVSwitchConfigManager::hexStringToBytes(const std::string& hexStr)
{
    if (hexStr.size() % 2 != 0)
    {
        throw std::invalid_argument("Hex string has odd length (" +
                                    std::to_string(hexStr.size()) +
                                    "); must be a multiple of 2.");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hexStr.size() / 2);

    for (std::size_t i = 0; i < hexStr.size(); i += 2)
    {
        unsigned char hi = static_cast<unsigned char>(hexStr[i]);
        unsigned char lo = static_cast<unsigned char>(hexStr[i + 1]);

        if (!std::isxdigit(hi) || !std::isxdigit(lo))
        {
            throw std::invalid_argument(
                "Non-hex character in blob string at byte offset " +
                std::to_string(i / 2) + " (chars '" + hexStr[i] +
                hexStr[i + 1] + "').");
        }

        uint8_t byte = 0;
        char pair[3] = {static_cast<char>(hi), static_cast<char>(lo), '\0'};
        auto [ptr, ec] = std::from_chars(pair, pair + 2, byte, 16);
        if (ec != std::errc{})
        {
            throw std::invalid_argument("Failed to parse hex pair at offset " +
                                        std::to_string(i));
        }
        bytes.push_back(byte);
    }

    return bytes;
}

/**
 * Parse a YAML config file whose structure is:
 *
 *   ports_down:          <- top-level key, no indentation
 *   - <hex_string>       <- list item at root indent (starts with "- ")
 *   configure:
 *   - <hex_string>
 *   ports_up:
 *   - <hex_string>
 *   metadata:
 *     ports_down_count: N    <- 2-space indented key: value
 *     ports_down_sizes:      <- key with sub-list
 *     - M                   <- 2-space indented list item
 *     configure_count: N
 *     configure_sizes:
 *     - M
 *     ports_up_count: N
 *     ports_up_sizes:
 *     - M
 *
 * Both hyphenated (ports-down) and underscored (ports_down) key names are
 * accepted throughout.  Blob ordering within each section is preserved.
 * Counts and sizes from metadata are validated against parsed blobs.
 */
SwitchConfig NVSwitchConfigManager::parseConfigFile(const std::string& content)
{
    enum class Section
    {
        None,
        PortsDown,
        Configure,
        PortsUp,
        Metadata
    };

    enum class MetaSub
    {
        None,
        PortsDownSizes,
        ConfigureSizes,
        PortsUpSizes
    };

    SwitchConfig result;

    int expectedPortsDown = -1;
    int expectedConfigure = -1;
    int expectedPortsUp = -1;
    std::vector<int> portsDownSizes;
    std::vector<int> configureSizes;
    std::vector<int> portsUpSizes;

    Section section = Section::None;
    MetaSub metaSub = MetaSub::None;

    std::istringstream iss(content);
    std::string line;
    int lineNum = 0;

    while (std::getline(iss, line))
    {
        ++lineNum;

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        bool isIndented = !line.empty() && (line[0] == ' ' || line[0] == '\t');

        std::string stripped = line;
        trimInPlace(stripped);

        if (stripped.empty() || stripped[0] == '#')
            continue;

        bool isListItem =
            stripped.size() >= 2 && stripped[0] == '-' && stripped[1] == ' ';
        bool hasColon = stripped.find(':') != std::string::npos;

        if (!isIndented)
        {
            if (isListItem)
            {
                if (section == Section::Metadata || section == Section::None)
                {
                    lg2::warning(
                        "parseConfigFile: root-level list item outside "
                        "a blob section at line {N}; ignored",
                        "N", static_cast<uint64_t>(lineNum));
                    continue;
                }

                std::string hexStr = stripped.substr(2);
                trimInPlace(hexStr);

                if (hexStr.empty())
                    throw std::runtime_error("Empty blob entry at line " +
                                             std::to_string(lineNum));

                auto bytes = hexStringToBytes(hexStr);

                switch (section)
                {
                    case Section::PortsDown:
                        result.portsDown.push_back(std::move(bytes));
                        break;
                    case Section::Configure:
                        result.configure.push_back(std::move(bytes));
                        break;
                    case Section::PortsUp:
                        result.portsUp.push_back(std::move(bytes));
                        break;
                    default:
                        break;
                }
            }
            else if (hasColon)
            {
                auto pos = stripped.find(':');
                std::string key = stripped.substr(0, pos);
                trimInPlace(key);

                metaSub = MetaSub::None;

                if (key == "ports_down" || key == "ports-down")
                    section = Section::PortsDown;
                else if (key == "configure")
                    section = Section::Configure;
                else if (key == "ports_up" || key == "ports-up")
                    section = Section::PortsUp;
                else if (key == "metadata")
                    section = Section::Metadata;
                else
                    lg2::warning(
                        "parseConfigFile: unknown top-level key '{KEY}' "
                        "at line {N}; ignored",
                        "KEY", key, "N", static_cast<uint64_t>(lineNum));
            }
        }
        else
        {
            if (section != Section::Metadata)
                continue;

            if (isListItem)
            {
                std::string valStr = stripped.substr(2);
                trimInPlace(valStr);

                if (valStr.empty())
                    throw std::runtime_error(
                        "Empty metadata list entry at line " +
                        std::to_string(lineNum));

                int val = 0;
                auto [ptr, ec] = std::from_chars(
                    valStr.data(), valStr.data() + valStr.size(), val);
                if (ec != std::errc{} || ptr != valStr.data() + valStr.size())
                    throw std::runtime_error(
                        "Non-integer value '" + valStr +
                        "' in metadata size list at line " +
                        std::to_string(lineNum));

                switch (metaSub)
                {
                    case MetaSub::PortsDownSizes:
                        portsDownSizes.push_back(val);
                        break;
                    case MetaSub::ConfigureSizes:
                        configureSizes.push_back(val);
                        break;
                    case MetaSub::PortsUpSizes:
                        portsUpSizes.push_back(val);
                        break;
                    case MetaSub::None:
                        lg2::warning(
                            "parseConfigFile: list item outside metadata "
                            "sub-key at line {N}; ignored",
                            "N", static_cast<uint64_t>(lineNum));
                        break;
                }
            }
            else if (hasColon)
            {
                auto pos = stripped.find(':');
                std::string key = stripped.substr(0, pos);
                std::string valStr = stripped.substr(pos + 1);
                trimInPlace(key);
                trimInPlace(valStr);

                metaSub = MetaSub::None;

                auto parseCount = [&](int& target, const char* name) {
                    if (valStr.empty())
                        throw std::runtime_error(std::string(name) +
                                                 " has no value at line " +
                                                 std::to_string(lineNum));
                    auto [ptr, ec] = std::from_chars(
                        valStr.data(), valStr.data() + valStr.size(), target);
                    if (ec != std::errc{} ||
                        ptr != valStr.data() + valStr.size())
                        throw std::runtime_error(
                            std::string("Invalid integer for ") + name +
                            " at line " + std::to_string(lineNum));
                };

                if (key == "ports_down_count" || key == "ports-down-count")
                    parseCount(expectedPortsDown, "ports_down_count");
                else if (key == "configure_count" || key == "configure-count")
                    parseCount(expectedConfigure, "configure_count");
                else if (key == "ports_up_count" || key == "ports-up-count")
                    parseCount(expectedPortsUp, "ports_up_count");
                else if (key == "ports_down_sizes" || key == "ports-down-sizes")
                    metaSub = MetaSub::PortsDownSizes;
                else if (key == "configure_sizes" || key == "configure-sizes")
                    metaSub = MetaSub::ConfigureSizes;
                else if (key == "ports_up_sizes" || key == "ports-up-sizes")
                    metaSub = MetaSub::PortsUpSizes;
                else
                    lg2::warning(
                        "parseConfigFile: unknown metadata key '{KEY}' "
                        "at line {N}; ignored",
                        "KEY", key, "N", static_cast<uint64_t>(lineNum));
            }
        }
    }

    lg2::debug("parseConfigFile: scan complete – "
               "ports_down={PD} configure={C} ports_up={PU}; "
               "expected pd={EPD} cfg={EC} pu={EPU}",
               "PD", static_cast<uint64_t>(result.portsDown.size()), "C",
               static_cast<uint64_t>(result.configure.size()), "PU",
               static_cast<uint64_t>(result.portsUp.size()), "EPD",
               expectedPortsDown, "EC", expectedConfigure, "EPU",
               expectedPortsUp);

    if (expectedPortsDown < 0 || expectedConfigure < 0 || expectedPortsUp < 0)
        throw std::runtime_error(
            "Config file is missing metadata or required count fields "
            "(ports_down_count / configure_count / ports_up_count).");

    if (static_cast<int>(result.portsDown.size()) != expectedPortsDown)
        throw std::runtime_error(
            "ports_down blob count mismatch: metadata says " +
            std::to_string(expectedPortsDown) + " but found " +
            std::to_string(result.portsDown.size()));

    if (static_cast<int>(result.configure.size()) != expectedConfigure)
        throw std::runtime_error(
            "configure blob count mismatch: metadata says " +
            std::to_string(expectedConfigure) + " but found " +
            std::to_string(result.configure.size()));

    if (static_cast<int>(result.portsUp.size()) != expectedPortsUp)
        throw std::runtime_error(
            "ports_up blob count mismatch: metadata says " +
            std::to_string(expectedPortsUp) + " but found " +
            std::to_string(result.portsUp.size()));

    auto validateSizes = [&](const std::vector<std::vector<uint8_t>>& blobs,
                             const std::vector<int>& sizes,
                             const char* sectionName) {
        if (sizes.empty())
            return;
        if (static_cast<int>(sizes.size()) != static_cast<int>(blobs.size()))
            throw std::runtime_error(
                std::string(sectionName) + " sizes list length (" +
                std::to_string(sizes.size()) + ") does not match blob count (" +
                std::to_string(blobs.size()) + ").");
        for (std::size_t i = 0; i < blobs.size(); ++i)
        {
            if (static_cast<int>(blobs[i].size()) != sizes[i])
                throw std::runtime_error(
                    std::string(sectionName) + "[" + std::to_string(i) +
                    "] size mismatch: metadata says " +
                    std::to_string(sizes[i]) + " bytes but hex decodes to " +
                    std::to_string(blobs[i].size()) + " bytes.");
        }
    };

    validateSizes(result.portsDown, portsDownSizes, "ports_down");
    validateSizes(result.configure, configureSizes, "configure");
    validateSizes(result.portsUp, portsUpSizes, "ports_up");
    return result;
}

int NVSwitchConfigManager::createMemfd(const std::vector<uint8_t>& data)
{
    int fd = ::memfd_create("nvswitch_blob", MFD_CLOEXEC);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(),
                                "memfd_create failed");

    if (!data.empty())
    {
        ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0 || static_cast<std::size_t>(written) != data.size())
        {
            int savedErrno = errno;
            ::close(fd);
            throw std::system_error(savedErrno, std::generic_category(),
                                    "write to memfd failed");
        }
    }

    if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1))
    {
        int savedErrno = errno;
        ::close(fd);
        throw std::system_error(savedErrno, std::generic_category(),
                                "lseek on memfd failed");
    }

    return fd;
}

} // namespace nvidia::nvswitch::config
