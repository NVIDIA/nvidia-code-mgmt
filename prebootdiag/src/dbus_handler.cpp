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
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace nvidia::prebootdiag
{

using namespace constants;

namespace
{

std::runtime_error dbusError(const std::string& operation,
                             const boost::system::error_code& ec)
{
    return std::runtime_error(operation + " failed: " + ec.message());
}

template <typename Result, typename... InputArgs>
boost::asio::awaitable<std::pair<boost::system::error_code, Result>>
    asyncMethodCall(std::shared_ptr<sdbusplus::asio::connection> bus,
                    const std::string& service, const std::string& path,
                    const std::string& iface, const std::string& method,
                    const InputArgs&... inputArgs)
{
    using ResultChannel = boost::asio::experimental::concurrent_channel<void(
        boost::system::error_code, Result)>;
    auto channel = std::make_shared<ResultChannel>(
        co_await boost::asio::this_coro::executor, 1);

    bus->async_method_call(
        [channel](boost::system::error_code ec, Result result) mutable {
            channel->try_send(ec, std::move(result));
        },
        service, path, iface, method, inputArgs...);

    auto [ec, result] = co_await channel->async_receive(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    co_return std::make_pair(ec, std::move(result));
}

template <typename... InputArgs>
boost::asio::awaitable<boost::system::error_code>
    asyncMethodCallNoReturn(std::shared_ptr<sdbusplus::asio::connection> bus,
                            const std::string& service, const std::string& path,
                            const std::string& iface, const std::string& method,
                            const InputArgs&... inputArgs)
{
    using ResultChannel = boost::asio::experimental::concurrent_channel<void(
        boost::system::error_code)>;
    auto channel = std::make_shared<ResultChannel>(
        co_await boost::asio::this_coro::executor, 1);

    bus->async_method_call(
        [channel](boost::system::error_code ec) mutable {
            channel->try_send(ec);
        },
        service, path, iface, method, inputArgs...);

    auto [ec] = co_await channel->async_receive(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    co_return ec;
}

std::string
    getStringPropertyValue(const std::variant<std::string>& propertyValue,
                           const std::string& operation)
{
    if (const auto* value = std::get_if<std::string>(&propertyValue))
    {
        return *value;
    }
    throw std::runtime_error(operation + " returned a non-string property");
}

} // namespace

SdbusHandler::SdbusHandler(std::shared_ptr<sdbusplus::asio::connection> bus) :
    bus(std::move(bus))
{}

boost::asio::awaitable<std::string>
    SdbusHandler::getSettingsStringProperty(const std::string& propName)
{
    const std::string operation = "Read Settings property " + propName;
    auto [ec, value] = co_await asyncMethodCall<std::variant<std::string>>(
        bus, settingsService, diagObjPath, propsIface, "Get", diagIface,
        propName);
    if (ec)
    {
        throw dbusError(operation, ec);
    }

    co_return getStringPropertyValue(value, operation);
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

    auto [ec, asyncObjPath] = co_await asyncMethodCall<sdbusplus::object_path>(
        bus, nsmService, path, nsmAsyncSetIface, "Set", iface,
        std::string{asyncValueProperty}, std::variant<std::string>{configJson});
    if (ec)
    {
        lg2::error("PreBootDiag: Async.Set on iface={IFACE} EID={EID} "
                   "failed: {ERR}",
                   "IFACE", iface, "EID", eid, "ERR", ec.message());
        co_return false;
    }

    lg2::info(
        "PreBootDiag: Async.Set issued for iface={IFACE} EID={EID}, watching path={PATH}",
        "IFACE", iface, "EID", eid, "PATH", asyncObjPath.str);

    co_return co_await waitForAsyncCompletion(asyncObjPath);
}

boost::asio::awaitable<bool>
    SdbusHandler::waitForAsyncCompletion(const sdbusplus::object_path& path)
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
    auto [statusEc, currentStatus] =
        co_await asyncMethodCall<std::variant<std::string>>(
            bus, nsmService, path.str, propsIface, "Get",
            std::string{nsmAsyncStatusIface}, std::string{"Status"});
    if (!statusEc)
    {
        if (const auto* statusStr = std::get_if<std::string>(&currentStatus))
        {
            resolveIfTerminal(*statusStr);
        }
    }
    else
    {
        lg2::warning(
            "PreBootDiag: post-match Get on Async.Status failed (will rely on signal): {ERR}",
            "ERR", statusEc.message());
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

boost::asio::awaitable<void>
    SdbusHandler::setSettingsStringProperty(const std::string& propName,
                                            const std::string& value)
{
    auto ec = co_await asyncMethodCallNoReturn(
        bus, settingsService, diagObjPath, propsIface, "Set", diagIface,
        propName, std::variant<std::string>(value));
    if (ec)
    {
        lg2::error("PreBootDiag: setSettingsStringProperty {PROP} failed: "
                   "{ERR}",
                   "PROP", propName, "ERR", ec.message());
        throw dbusError("Set Settings property " + propName, ec);
    }
    co_return;
}

boost::asio::awaitable<DiagStatus> SdbusHandler::getDiagStatus()
{
    auto [ec, value] = co_await asyncMethodCall<std::variant<uint8_t>>(
        bus, settingsService, diagObjPath, propsIface, "Get", diagIface,
        "DiagStatus");
    if (ec)
    {
        lg2::warning("PreBootDiag: Failed to read DiagStatus: {ERR}", "ERR",
                     ec.message());
        co_return DiagStatus::NotStarted;
    }

    const auto* status = std::get_if<uint8_t>(&value);
    if (status == nullptr)
    {
        lg2::warning("PreBootDiag: DiagStatus D-Bus reply had invalid type");
        co_return DiagStatus::NotStarted;
    }
    co_return static_cast<DiagStatus>(*status);
}

boost::asio::awaitable<void> SdbusHandler::setDiagStatus(DiagStatus status)
{
    auto ec = co_await asyncMethodCallNoReturn(
        bus, settingsService, diagObjPath, propsIface, "Set", diagIface,
        "DiagStatus", std::variant<uint8_t>(static_cast<uint8_t>(status)));
    if (ec)
    {
        lg2::error("PreBootDiag: Failed to set DiagStatus: {ERR}", "ERR",
                   ec.message());
        throw dbusError("Set DiagStatus", ec);
    }
    co_return;
}

boost::asio::awaitable<void> SdbusHandler::setDiagMode(bool mode)
{
    auto ec = co_await asyncMethodCallNoReturn(
        bus, settingsService, diagObjPath, propsIface, "Set", diagIface,
        "DiagMode", std::variant<bool>(mode));
    if (ec)
    {
        lg2::error("PreBootDiag: Failed to set DiagMode: {ERR}", "ERR",
                   ec.message());
        throw dbusError("Set DiagMode", ec);
    }
    co_return;
}

boost::asio::awaitable<bool> SdbusHandler::hasDiagConfig()
{
    auto [ec, value] = co_await asyncMethodCall<std::variant<std::string>>(
        bus, settingsService, diagObjPath, propsIface, "Get", diagIface,
        "DiagConfig");
    if (ec)
    {
        lg2::warning("PreBootDiag: Failed to read DiagConfig: {ERR}", "ERR",
                     ec.message());
        co_return false;
    }

    const auto* config = std::get_if<std::string>(&value);
    if (config == nullptr)
    {
        lg2::warning("PreBootDiag: DiagConfig D-Bus reply had invalid type");
        co_return false;
    }
    co_return !config->empty() && *config != "[]";
}

boost::asio::awaitable<void>
    SdbusHandler::createErrorLog(const std::string& message,
                                 const std::string& additionalInfo)
{
    std::map<std::string, std::string> additionalData;
    additionalData["PACKAGE_ID"] = additionalInfo;

    auto ec = co_await asyncMethodCallNoReturn(
        bus, logService, logObjPath, logIface, "Create", message,
        "xyz.openbmc_project.Logging.Entry.Level.Error", additionalData);
    if (ec)
    {
        lg2::error("PreBootDiag: Failed to create log entry: {ERR}", "ERR",
                   ec.message());
    }
    co_return;
}

} // namespace nvidia::prebootdiag
