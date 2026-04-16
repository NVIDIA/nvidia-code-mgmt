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

#include <systemd/sd-daemon.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <functional>

int main()
{
    using namespace nvidia::nvswitch::config;

    try
    {
        boost::asio::io_context ioc;

        auto conn = std::make_shared<sdbusplus::asio::connection>(ioc);
        conn->request_name(busName);

        lg2::info("nvswitch-config-manager: starting, bus name '{BUS}'", "BUS",
                  busName);

        NVSwitchConfigManager manager(ioc, conn);

        // Watchdog: if WatchdogSec= is set in the unit file,
        // sd_watchdog_enabled() returns the interval. We ping at half that
        // interval – same principle as sdeventplus::Event::set_watchdog(true)
        // used by nsmd, but driven by the boost::asio event loop we already
        // have.
        uint64_t wdUsec = 0;
        boost::asio::steady_timer wdTimer(ioc);
        std::function<void()> petWatchdog = [&]() {
            sd_notify(0, "WATCHDOG=1");
            wdTimer.expires_after(std::chrono::microseconds(wdUsec / 2));
            wdTimer.async_wait([&](boost::system::error_code ec) {
                if (!ec)
                    petWatchdog();
            });
        };
        if (sd_watchdog_enabled(0, &wdUsec) > 0)
            petWatchdog();

        ioc.run();
    }
    catch (const std::exception& e)
    {
        lg2::error("nvswitch-config-manager: fatal error: {ERR}", "ERR",
                   e.what());
        return 1;
    }

    return 0;
}
