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

#include "dbus_handler.hpp"

#include <gmock/gmock.h>

namespace nvidia::prebootdiag
{

/// @brief Mock D-Bus handler - wraps synchronous gmock methods into
/// coroutines. NSM events are no longer property-watched; they arrive via
/// the App.Notify method on PreBootDiag itself, so the previous
/// event-queue plumbing has been removed.
class MockDbusHandler : public DbusHandlerInterface
{
  public:
    MOCK_METHOD(std::string, mockGetSettingsStringProperty,
                (const std::string& propName));
    MOCK_METHOD(bool, mockCallNsmConfigSetSystem,
                (uint8_t eid, const std::string& configJson));
    MOCK_METHOD(bool, mockCallNsmConfigSetTID,
                (uint8_t eid, const std::string& configJson));
    MOCK_METHOD(void, mockSetSettingsStringProperty,
                (const std::string& propName, const std::string& value));
    MOCK_METHOD(DiagStatus, mockGetDiagStatus, ());
    MOCK_METHOD(void, mockSetDiagStatus, (DiagStatus status));
    MOCK_METHOD(void, mockSetDiagMode, (bool mode));
    MOCK_METHOD(bool, mockHasDiagConfig, ());
    MOCK_METHOD(void, mockCreateErrorLog,
                (const std::string& message,
                 const std::string& additionalInfo));

    boost::asio::awaitable<std::string>
        getSettingsStringProperty(const std::string& propName) override
    {
        co_return mockGetSettingsStringProperty(propName);
    }

    boost::asio::awaitable<bool>
        callNsmConfigSetSystem(uint8_t eid,
                               const std::string& configJson) override
    {
        co_return mockCallNsmConfigSetSystem(eid, configJson);
    }

    boost::asio::awaitable<bool>
        callNsmConfigSetTID(uint8_t eid, const std::string& configJson) override
    {
        co_return mockCallNsmConfigSetTID(eid, configJson);
    }

    boost::asio::awaitable<void>
        setSettingsStringProperty(const std::string& propName,
                                  const std::string& value) override
    {
        mockSetSettingsStringProperty(propName, value);
        co_return;
    }

    boost::asio::awaitable<DiagStatus> getDiagStatus() override
    {
        co_return mockGetDiagStatus();
    }

    boost::asio::awaitable<void> setDiagStatus(DiagStatus status) override
    {
        mockSetDiagStatus(status);
        co_return;
    }

    boost::asio::awaitable<void> setDiagMode(bool mode) override
    {
        mockSetDiagMode(mode);
        co_return;
    }

    boost::asio::awaitable<bool> hasDiagConfig() override
    {
        co_return mockHasDiagConfig();
    }

    boost::asio::awaitable<void>
        createErrorLog(const std::string& message,
                       const std::string& additionalInfo) override
    {
        mockCreateErrorLog(message, additionalInfo);
        co_return;
    }
};

} // namespace nvidia::prebootdiag
