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

#include <boost/asio.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message/types.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace nvidia::prebootdiag
{

using constants::DiagStatus;

class DbusHandlerInterface
{
  public:
    virtual ~DbusHandlerInterface() = default;

    virtual std::string
        getSettingsStringProperty(const std::string& propName) = 0;

    /// Push a SystemConfig payload to nsmd via com.nvidia.Async.Set on the
    /// com.nvidia.PreBootDiag.Config.System interface, then await
    /// completion via the returned object_path's com.nvidia.Async.Status.
    /// Returns true on Success, false on any non-Success terminal status
    /// or D-Bus error.
    virtual boost::asio::awaitable<bool>
        callNsmConfigSetSystem(uint8_t eid, const std::string& configJson) = 0;

    /// Same as callNsmConfigSetSystem but for the
    /// com.nvidia.PreBootDiag.Config.TID interface.
    virtual boost::asio::awaitable<bool>
        callNsmConfigSetTID(uint8_t eid, const std::string& configJson) = 0;

    virtual void setSettingsStringProperty(const std::string& propName,
                                           const std::string& value) = 0;

    virtual DiagStatus getDiagStatus() = 0;

    virtual void setDiagStatus(DiagStatus status) = 0;

    virtual void setDiagMode(bool mode) = 0;

    virtual bool hasDiagConfig() = 0;

    virtual void createErrorLog(const std::string& message,
                                const std::string& additionalInfo) = 0;
};

class SdbusHandler : public DbusHandlerInterface
{
  public:
    explicit SdbusHandler(std::shared_ptr<sdbusplus::asio::connection> bus);

    std::string getSettingsStringProperty(const std::string& propName) override;

    boost::asio::awaitable<bool>
        callNsmConfigSetSystem(uint8_t eid,
                               const std::string& configJson) override;

    boost::asio::awaitable<bool>
        callNsmConfigSetTID(uint8_t eid,
                            const std::string& configJson) override;

    void setSettingsStringProperty(const std::string& propName,
                                   const std::string& value) override;

    DiagStatus getDiagStatus() override;

    void setDiagStatus(DiagStatus status) override;

    void setDiagMode(bool mode) override;

    bool hasDiagConfig() override;

    void createErrorLog(const std::string& message,
                        const std::string& additionalInfo) override;

  private:
    /// Shared body for callNsmConfigSetSystem and callNsmConfigSetTID. Issues
    /// a com.nvidia.Async.Set("Value", configJson) on the given iface, reads
    /// the returned object_path, and waits for the Status property to leave
    /// InProgress.
    boost::asio::awaitable<bool> callAsyncSet(uint8_t eid,
                                              const std::string& iface,
                                              const std::string& configJson);

    /// Block until the com.nvidia.Async.Status.Status property at `path`
    /// transitions to a terminal state. Returns true iff it lands on
    /// "Success".
    boost::asio::awaitable<bool>
        waitForAsyncCompletion(const sdbusplus::message::object_path& path);

    std::shared_ptr<sdbusplus::asio::connection> bus;
};

} // namespace nvidia::prebootdiag
