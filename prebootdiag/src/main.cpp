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
#include "dbus_handler.hpp"
#include "gpio_handler.hpp"
#include "prebootdiag.hpp"

#include <boost/asio.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <csignal>

int main()
{
    boost::asio::io_context io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    bus->request_name("com.nvidia.PreBootDiag");
    sdbusplus::asio::object_server server(bus);

    auto gpio = std::make_unique<nvidia::prebootdiag::LibGpioHandler>();
    auto dbus = std::make_unique<nvidia::prebootdiag::SdbusHandler>(bus);

    nvidia::prebootdiag::PreBootDiag diag(io, bus, server, std::move(gpio),
                                          std::move(dbus));

    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait(
        [&](const boost::system::error_code&, int) { io.stop(); });

    lg2::info("PreBootDiag: Service started");
    io.run();
    return 0;
}
