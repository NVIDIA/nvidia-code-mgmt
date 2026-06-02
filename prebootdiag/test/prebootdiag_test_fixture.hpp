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
#include "mock_dbus_handler.hpp"
#include "mock_gpio_handler.hpp"
#include "prebootdiag.hpp"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <string>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nvidia::prebootdiag
{

class PreBootDiagFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        bus = std::make_shared<sdbusplus::asio::connection>(io);
        server = std::make_unique<sdbusplus::asio::object_server>(bus);

        mockGpioOwned = std::make_unique<MockGpioHandler>();
        mockDbusOwned = std::make_unique<MockDbusHandler>();
        mockGpio = mockGpioOwned.get();
        mockDbus = mockDbusOwned.get();

        // Permissive default so tests that don't care about the GPIO
        // sequence still pass; tests that DO care override with EXPECT_CALL.
        ON_CALL(*mockGpio, setPin(::testing::_, ::testing::_))
            .WillByDefault(::testing::Return(std::error_code{}));
        ON_CALL(*mockGpio, holdPin(::testing::_, ::testing::_))
            .WillByDefault(::testing::Return(std::error_code{}));
        ON_CALL(*mockGpio, releaseAllPins()).WillByDefault(::testing::Return());
        ON_CALL(*mockGpio, releasePins(::testing::_))
            .WillByDefault(::testing::Return());
    }

    std::unique_ptr<PreBootDiag> create()
    {
        return std::make_unique<PreBootDiag>(io, bus, *server,
                                             std::move(mockGpioOwned),
                                             std::move(mockDbusOwned));
    }

    /// Create the service and drive it through Enable=true plus the two
    /// ordered post code gates so the coroutine lands inside
    /// listenForEvents. NSM events for specific tests should be pushed
    /// via sendStateUpdate().
    std::unique_ptr<PreBootDiag> createAndStart(bool passGates = true)
    {
        auto diag = create();
        setEnabled(true);
        if (passGates)
        {
            sendPostCode(constants::postCodePscFmcBootModePrebootDiag);
            sendPostCode(constants::postCodeMb2CcplexPrebootDiagEntry);
        }
        return diag;
    }

    void sendPostCode(const std::string& name)
    {
        nlohmann::json j;
        j["PostCodeName"] = name;
        sendStateUpdate(constants::statePostCodeReceived, j.dump());
    }

    void setEnabled(bool value)
    {
        auto method = bus->new_method_call(bus->get_unique_name().c_str(),
                                           constants::appObjPath,
                                           constants::propsIface, "Set");
        method.append(constants::appEnableIface, "Enabled",
                      std::variant<bool>(value));
        bus->call(method);
    }

    void sendStateUpdate(const std::string& state,
                         const std::string& payload = "")
    {
        auto method = bus->new_method_call(bus->get_unique_name().c_str(),
                                           constants::appObjPath,
                                           constants::appNotifyIface, "Notify");
        method.append(state, payload);
        bus->call(method);
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::unique_ptr<sdbusplus::asio::object_server> server;

    // Owning unique_ptrs; transferred into PreBootDiag via std::move in
    // create(). Non-owning raw pointers (mockGpio/mockDbus) stay valid for
    // EXPECT_CALL until PreBootDiag is destroyed.
    std::unique_ptr<MockGpioHandler> mockGpioOwned;
    std::unique_ptr<MockDbusHandler> mockDbusOwned;
    MockGpioHandler* mockGpio = nullptr;
    MockDbusHandler* mockDbus = nullptr;
};

} // namespace nvidia::prebootdiag
