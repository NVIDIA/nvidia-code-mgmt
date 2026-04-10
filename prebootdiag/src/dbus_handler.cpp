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

#include "constants.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <map>
#include <variant>

namespace nvidia::prebootdiag
{

using namespace constants;

SdbusHandler::SdbusHandler(std::shared_ptr<sdbusplus::asio::connection> bus) :
    bus(std::move(bus))
{}

std::string SdbusHandler::getSettingsStringProperty(const std::string& propName)
{
    auto method =
        bus->new_method_call(settingsService, diagObjPath, propsIface, "Get");
    method.append(diagIface, propName);
    auto reply = bus->call(method);

    std::variant<std::string> value;
    reply.read(value);
    return std::get<std::string>(value);
}

boost::asio::awaitable<bool>
    SdbusHandler::callNsmConfigSetSystem(uint8_t eid,
                                         const std::string& configJson)
{
    co_return co_await callAsyncSet(eid, nsmConfigSystemIface, configJson);
}

boost::asio::awaitable<bool>
    SdbusHandler::callNsmConfigSetTID(uint8_t eid,
                                      const std::string& configJson)
{
    co_return co_await callAsyncSet(eid, nsmConfigTidIface, configJson);
}

boost::asio::awaitable<bool>
    SdbusHandler::callAsyncSet(uint8_t eid, const std::string& iface,
                               const std::string& configJson)
{
    auto path = std::string(nsmObjPathPrefix) + std::to_string(eid);

    sdbusplus::message::object_path asyncObjPath;
    try
    {
        auto method = bus->new_method_call(nsmService, path.c_str(),
                                           nsmAsyncSetIface, "Set");
        method.append(iface, std::string{asyncValueProperty},
                      std::variant<std::string>{configJson});
        auto reply = bus->call(method);
        reply.read(asyncObjPath);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error(
            "PreBootDiag: Async.Set on iface={IFACE} EID={EID} failed: {ERR}",
            "IFACE", iface, "EID", eid, "ERR", e.what());
        co_return false;
    }

    lg2::info(
        "PreBootDiag: Async.Set issued for iface={IFACE} EID={EID}, watching path={PATH}",
        "IFACE", iface, "EID", eid, "PATH", asyncObjPath.str);

    co_return co_await waitForAsyncCompletion(asyncObjPath);
}

boost::asio::awaitable<bool> SdbusHandler::waitForAsyncCompletion(
    const sdbusplus::message::object_path& path)
{
    using ResultChannel = boost::asio::experimental::concurrent_channel<void(
        boost::system::error_code, bool)>;
    auto promise = std::make_shared<ResultChannel>(
        co_await boost::asio::this_coro::executor, 1);

    // Single classifier for both the signal-driven and Get-driven paths.
    // Strips the qualified-name prefix (e.g.
    // "...AsyncOperationStatus.Success" -> "Success"), and on a terminal
    // value try_sends the result. Capacity-1 channel + try_send guarantees
    // at-most-once resolution if both paths fire.
    auto resolveIfTerminal = [promise](const std::string& fullStatus) -> bool {
        auto pos = fullStatus.find_last_of('.');
        std::string status = (pos == std::string::npos)
                                 ? fullStatus
                                 : fullStatus.substr(pos + 1);
        if (status == "InProgress")
        {
            return false;
        }
        promise->try_send(boost::system::error_code{}, status == "Success");
        return true;
    };

    std::string matchRule = sdbusplus::bus::match::rules::propertiesChanged(
        path.str, nsmAsyncStatusIface);

    auto match = std::make_shared<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*bus), matchRule.c_str(),
        [resolveIfTerminal](sdbusplus::message_t& msg) {
            std::string iface;
            std::map<std::string, std::variant<std::string>> props;
            try
            {
                msg.read(iface, props);
            }
            catch (const std::exception& e)
            {
                lg2::error("PreBootDiag: Async.Status read failed: {ERR}",
                           "ERR", e.what());
                return;
            }
            auto it = props.find("Status");
            if (it == props.end())
            {
                return;
            }
            const auto* statusStr = std::get_if<std::string>(&it->second);
            if (!statusStr)
            {
                return;
            }
            resolveIfTerminal(*statusStr);
        });

    // Defense-in-depth Get: handles the race where the dispatcher's
    // setImpl() wrote the terminal Status *before* our match was
    // registered above. The PropertiesChanged signal in that case has
    // already fired with no listener, but the property reflects the
    // terminal value — read it explicitly. Duplicate resolution from
    // match-then-Get (or vice versa) is harmless thanks to try_send.
    try
    {
        auto getMethod =
            bus->new_method_call(nsmService, path.str.c_str(),
                                 "org.freedesktop.DBus.Properties", "Get");
        getMethod.append(std::string{nsmAsyncStatusIface},
                         std::string{"Status"});
        auto reply = bus->call(getMethod);
        std::variant<std::string> currentStatus;
        reply.read(currentStatus);
        if (const auto* statusStr = std::get_if<std::string>(&currentStatus))
        {
            resolveIfTerminal(*statusStr);
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "PreBootDiag: post-match Get on Async.Status failed (will rely on signal): {ERR}",
            "ERR", e.what());
    }

    auto [ec, success] = co_await promise->async_receive(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    if (ec)
    {
        lg2::error("PreBootDiag: Async.Status wait failed: {ERR}", "ERR",
                   ec.message());
        co_return false;
    }
    co_return success;
}

void SdbusHandler::setSettingsStringProperty(const std::string& propName,
                                             const std::string& value)
{
    try
    {
        auto method = bus->new_method_call(settingsService, diagObjPath,
                                           propsIface, "Set");
        method.append(diagIface, propName, std::variant<std::string>(value));
        bus->call_noreply(method);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "PreBootDiag: setSettingsStringProperty {PROP} failed: {ERR}",
            "PROP", propName, "ERR", e.what());
        throw;
    }
}

DiagStatus SdbusHandler::getDiagStatus()
{
    try
    {
        auto method = bus->new_method_call(settingsService, diagObjPath,
                                           propsIface, "Get");
        method.append(diagIface, "DiagStatus");
        auto reply = bus->call(method);

        std::variant<uint8_t> value;
        reply.read(value);
        return static_cast<DiagStatus>(std::get<uint8_t>(value));
    }
    catch (const std::exception& e)
    {
        lg2::warning("PreBootDiag: Failed to read DiagStatus: {ERR}", "ERR",
                     e.what());
        return DiagStatus::NotStarted;
    }
}

void SdbusHandler::setDiagStatus(DiagStatus status)
{
    auto method =
        bus->new_method_call(settingsService, diagObjPath, propsIface, "Set");
    method.append(diagIface, "DiagStatus",
                  std::variant<uint8_t>(static_cast<uint8_t>(status)));
    bus->call_noreply(method);
}

void SdbusHandler::setDiagMode(bool mode)
{
    auto method =
        bus->new_method_call(settingsService, diagObjPath, propsIface, "Set");
    method.append(diagIface, "DiagMode", std::variant<bool>(mode));
    bus->call_noreply(method);
}

bool SdbusHandler::hasDiagConfig()
{
    try
    {
        auto method = bus->new_method_call(settingsService, diagObjPath,
                                           propsIface, "Get");
        method.append(diagIface, "DiagConfig");
        auto reply = bus->call(method);

        std::variant<std::string> value;
        reply.read(value);
        const auto& config = std::get<std::string>(value);
        return !config.empty() && config != "[]";
    }
    catch (const std::exception& e)
    {
        lg2::warning("PreBootDiag: Failed to read DiagConfig: {ERR}", "ERR",
                     e.what());
        return false;
    }
}

void SdbusHandler::createErrorLog(const std::string& message,
                                  const std::string& additionalInfo)
{
    try
    {
        std::map<std::string, std::string> additionalData;
        additionalData["PACKAGE_ID"] = additionalInfo;

        auto method =
            bus->new_method_call(logService, logObjPath, logIface, "Create");
        method.append(message, "xyz.openbmc_project.Logging.Entry.Level.Error",
                      additionalData);
        bus->call_noreply(method);
    }
    catch (const std::exception& e)
    {
        lg2::error("PreBootDiag: Failed to create log entry: {ERR}", "ERR",
                   e.what());
    }
}

} // namespace nvidia::prebootdiag
