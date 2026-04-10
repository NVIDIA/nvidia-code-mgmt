/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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

#include "constants.hpp"
#include "dbus_handler.hpp"
#include "gpio_handler.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace nvidia::prebootdiag
{

/// Lifecycle states carried by com.nvidia.PreBootDiag.App.Notify.
/// Values match the fully-qualified enum strings in constants.hpp.
enum class StateType : uint8_t
{
    PostCodeReceived,
    SystemConfigRequested,
    TIDConfigRequested,
    HeartbeatReceived,
    ResultReceived,
    SessionEnded,
};

class PreBootDiag
{
  public:
    PreBootDiag(boost::asio::io_context& io,
                std::shared_ptr<sdbusplus::asio::connection> bus,
                sdbusplus::asio::object_server& server,
                std::unique_ptr<GpioHandlerInterface> gpio,
                std::unique_ptr<DbusHandlerInterface> dbus);
    ~PreBootDiag() = default;

    PreBootDiag(const PreBootDiag&) = delete;
    PreBootDiag& operator=(const PreBootDiag&) = delete;
    PreBootDiag(PreBootDiag&&) = delete;
    PreBootDiag& operator=(PreBootDiag&&) = delete;

    // Test hook: override both post-code wait timeouts to the same short
    // value. Tests don't exercise the per-phase boundary, so a single
    // setter keeps call sites trivial.
    void setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds d)
    {
        pscFmcWaitDuration = d;
        mb2WaitDuration = d;
    }

  private:
    bool setEnabled(const bool& requested, bool& current);
    void updateState(const std::string& state, const std::string& payload);

    boost::asio::awaitable<void> runDiagnosticSession();
    void initGpioSequence();
    boost::asio::awaitable<void> waitForPostCodes();
    boost::asio::awaitable<void> listenForEvents();
    void recoverCpus();

    // Best-effort: clear both CPU boot-chain GPIOs; logs any errors,
    // never throws — safe to call from recovery paths.
    void clearBootChainGpios();

    boost::asio::awaitable<void>
        handleSystemConfigRequested(const std::string& payload);
    boost::asio::awaitable<void>
        handleTIDConfigRequested(const std::string& payload);
    void handleResultReceived(const std::string& payload);

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::dbus_interface> enableIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateIface;

    bool enabled = false;
    bool sequenceRunning = false;
    bool abortRequested = false;
    // Specific reason recorded when abortRequested is set programmatically
    // (e.g. out-of-order post code). Empty for a user-driven Disable;
    // waitForPostCodes / listenForEvents use this to surface a precise
    // failure string into the error-log recovery flow.
    std::string abortReason;

    // Ordered gate flags, reset at each session start. Both must be set
    // (in order: PSC-FMC then MB2) before listenForEvents() runs.
    bool pscFmcPostCodeReceived = false;
    bool mb2PostCodeReceived = false;

    std::chrono::milliseconds pscFmcWaitDuration{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            constants::pscFmcPostCodeWaitTimeout)};
    std::chrono::milliseconds mb2WaitDuration{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            constants::mb2PostCodeWaitTimeout)};
    std::unique_ptr<boost::asio::steady_timer> gateWaitTimer;

    /// Push-driven event queue for NSM events during listenForEvents.
    /// Populated by updateState(); consumed by listenForEvents.
    struct EventEntry
    {
        StateType state = StateType::SessionEnded;
        std::string payload;
    };
    using EventChannel = boost::asio::experimental::channel<void(
        boost::system::error_code, EventEntry)>;
    std::unique_ptr<EventChannel> eventChannel;

    std::unique_ptr<GpioHandlerInterface> gpio;
    std::unique_ptr<DbusHandlerInterface> dbus;
};

} // namespace nvidia::prebootdiag
