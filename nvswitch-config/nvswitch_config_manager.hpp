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

#pragma once

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nvidia::nvswitch::config
{

// ---- DBus service identity ----
inline constexpr auto busName = "com.Nvidia.Software.NVSwitchConfig.Updater";
inline constexpr auto managerObjectPath = "/com/nvidia/NVSwitchConfig";
inline constexpr auto managerInterface = "com.nvidia.SwitchConfig.Updater";

// ---- Config file storage ----
inline constexpr auto configDir = "/var/emmc/misc/switch-config";
inline constexpr auto configFileName = "nvswitch-nvlink-config.yaml";

// Max config file size (64 KB; expected max ~2 KB as per spec)
inline constexpr std::size_t maxConfigFileSize = 64 * 1024;

// ---- NVSwitch device signal source ----
inline constexpr auto requestConfigIface =
    "com.nvidia.DeviceConfiguration.ConfigRequester";
inline constexpr auto configUpdateSignal = "DeviceConfigurationRequested";

// ---- SetDeviceConfiguration dbus call source ----
inline constexpr auto deviceConfigIface =
    "com.nvidia.DeviceConfiguration.ConfigUpdater";
inline constexpr auto setDeviceConfigMethod = "SetDeviceConfiguration";
// ConfigurationType enum value: NVLinkAccessRegister (value 0)
inline constexpr auto nvlinkAccessConfigType =
    "com.nvidia.DeviceConfiguration.ConfigUpdater.ConfigurationType.NVLinkAccessRegister";

// ---- Async status/value interfaces returned by SetDeviceConfiguration ----
// The returned object_path exposes com.nvidia.Async.Status to signal
// completion and carry results.
inline constexpr auto asyncStatusIface = "com.nvidia.Async.Status";
inline constexpr auto asyncValueIface = "com.nvidia.Async.Value";

// ---- ObjectMapper ----
inline constexpr auto objectMapperBusName = "xyz.openbmc_project.ObjectMapper";
inline constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
inline constexpr auto objectMapperIface = "xyz.openbmc_project.ObjectMapper";

// ---- Parsed config structure ----
// Blobs are stored as raw byte vectors; ordering within each section
// matches the order they must be sent to the device.
struct SwitchConfig
{
    std::vector<std::vector<uint8_t>> portsDown;
    std::vector<std::vector<uint8_t>> configure;
    std::vector<std::vector<uint8_t>> portsUp;
};

/**
 * @brief Manages OOB configuration delivery to NVSwitch QM devices.
 *
 * Phase 1 – Exposes AddConfigFile / RemoveConfigFile dbus methods so that
 * bmcweb (Redfish back-end) can persist a YAML configuration file to eMMC.
 *
 * Phase 2 – Watches the DeviceConfigurationRequested signal emitted by each
 * NVSwitch device after reset/reboot.  On receipt the manager reads the
 * stored config and pushes every blob to the device using the
 * SetDeviceConfiguration dbus call, one blob per call.
 *
 * Implementation is fully single-threaded, driven by the shared boost::asio
 * io_context.  All D-Bus calls use async_method_call; async-op polling uses
 * boost::asio::steady_timer rather than a blocking loop.  No mutexes or
 * worker threads are required.
 */
class NVSwitchConfigManager
{
  public:
    explicit NVSwitchConfigManager(
        boost::asio::io_context& ioc,
        std::shared_ptr<sdbusplus::asio::connection> conn);

    ~NVSwitchConfigManager() = default;
    NVSwitchConfigManager(const NVSwitchConfigManager&) = delete;
    NVSwitchConfigManager& operator=(const NVSwitchConfigManager&) = delete;
    NVSwitchConfigManager(NVSwitchConfigManager&&) = delete;
    NVSwitchConfigManager& operator=(NVSwitchConfigManager&&) = delete;

  private:
    boost::asio::io_context& ioc_;
    std::shared_ptr<sdbusplus::asio::connection> conn_;
    sdbusplus::asio::object_server server_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface_;

    struct DeviceState
    {
        bool inProgress{false};
    };
    std::map<std::string, std::shared_ptr<DeviceState>> deviceStates_;

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> signalMatches_;

    struct BlobSendContext;

    // ---- Phase 1: dbus method handlers (io_context thread) ----
    void handleAddConfigFile(const sdbusplus::message::unix_fd& fileFd);
    void handleRemoveConfigFile();

    // ---- Phase 2: signal handling ----
    void setupSignalWatchers();

    /** Async: query ObjectMapper GetSubTree for all objects implementing
     *  requestConfigIface and call subscribeToDevice for each result.
     *  Safe to call multiple times; already-subscribed paths are skipped. */
    void discoverAndSubscribeDevices();

    /** Register a DeviceState entry and a DeviceConfigurationRequested signal
     *  match for devicePath.  No-op if the path is already subscribed. */
    void subscribeToDevice(const std::string& devicePath);

    /** Called when org.freedesktop.DBus.ObjectManager.InterfacesAdded fires.
     *  Checks the added interface list; if requestConfigIface is present,
     *  calls subscribeToDevice directly — no ObjectMapper round-trip needed. */
    void onInterfacesAdded(sdbusplus::message::message& msg);

    /** Called on io_context thread when a signal arrives for devicePath.
     *  Starts the async config-apply chain if not already in progress. */
    void onConfigUpdateSignal(const std::string& devicePath);

    // ---- Phase 2: async config-application chain ----

    /** Entry point: read+parse config synchronously, then resolve the
     *  service name via an async GetObject call, then start sendNextBlob. */
    void applyConfigToDevice(const std::string& devicePath);

    /** Send ctx->blobs[ctx->current] via an async SetDeviceConfiguration
     *  call, then hand off to pollBlobStatus. */
    void sendNextBlob(std::shared_ptr<BlobSendContext> ctx);

    /** Poll com.nvidia.Async.Status at asyncPath using steady_timer until
     *  the operation completes or times out, then advance to sendNextBlob. */
    void pollBlobStatus(std::shared_ptr<BlobSendContext> ctx,
                        std::string asyncPath, unsigned int elapsedMs);

    /** Called when all blobs have been processed.  Clears inProgress. */
    void finishApply(std::shared_ptr<BlobSendContext> ctx);

    // ---- Static helpers ----
    static std::filesystem::path getConfigFilePath();
    static std::string readFromFd(int fd);
    static SwitchConfig parseConfigFile(const std::string& content);
    static std::vector<uint8_t> hexStringToBytes(const std::string& hexStr);
    static int createMemfd(const std::vector<uint8_t>& data);
    static void trimInPlace(std::string& s); /** Trim leading and trailing ASCII
                                                whitespace from s in-place. */
};

} // namespace nvidia::nvswitch::config
