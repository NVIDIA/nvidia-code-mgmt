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
#include "prebootdiag.hpp"

#include "constants.hpp"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <stdexcept>
#include <utility>

namespace nvidia::prebootdiag
{

using namespace constants;

PreBootDiag::PreBootDiag(boost::asio::io_context& io,
                         std::shared_ptr<sdbusplus::asio::connection> bus,
                         sdbusplus::asio::object_server& server,
                         std::unique_ptr<GpioHandlerInterface> gpio,
                         std::unique_ptr<DbusHandlerInterface> dbus) :
    io(io), bus(std::move(bus)), gpio(std::move(gpio)), dbus(std::move(dbus))
{
    enableIface = server.add_interface(appObjPath, appEnableIface);
    enableIface->register_property(
        "Enabled", false, [this](const bool& requested, bool& current) {
            return setEnabled(requested, current);
        });
    enableIface->initialize();

    stateIface = server.add_interface(appObjPath, appNotifyIface);
    stateIface->register_method(
        "Notify", [this](const std::string& state, const std::string& payload) {
            updateState(state, payload);
        });
    stateIface->initialize();

    lg2::info("PreBootDiag: Registered at {PATH}", "PATH", appObjPath);
}

bool PreBootDiag::setEnabled(const bool& requested, bool& current)
{
    if (requested == enabled)
    {
        current = requested;
        return true;
    }

    if (requested)
    {
        if (sequenceRunning)
        {
            lg2::warning(
                "PreBootDiag: Enable rejected, session already running");
            throw sdbusplus::exception::SdBusError(
                -EBUSY, "Diagnostic boot sequence already in progress");
        }

        enabled = true;
        current = true;
        sequenceRunning = true;
        abortRequested = false;
        abortReason.clear();
        pscFmcPostCodeReceived = false;
        mb2PostCodeReceived = false;
        eventChannel = std::make_unique<EventChannel>(io, 32);

        lg2::info("PreBootDiag: Enabled -> starting diagnostic session");
        boost::asio::co_spawn(
            io,
            [this]() -> boost::asio::awaitable<void> {
                co_await runDiagnosticSession();
            },
            boost::asio::detached);
    }
    else
    {
        if (!sequenceRunning)
        {
            enabled = false;
            current = false;
            return true;
        }

        lg2::info("PreBootDiag: Disabled -> aborting diagnostic session");
        enabled = false;
        current = false;
        abortRequested = true;
        if (gateWaitTimer)
        {
            gateWaitTimer->cancel();
        }
        if (eventChannel)
        {
            // Push a sentinel so listenForEvents wakes up.
            eventChannel->try_send(boost::system::error_code{},
                                   EventEntry{StateType::SessionEnded, ""});
        }
    }

    return true;
}

void PreBootDiag::updateState(const std::string& state,
                              const std::string& payload)
{
    if (!sequenceRunning)
    {
        lg2::warning(
            "PreBootDiag: App.Notify rejected, no session running ({S})", "S",
            state);
        throw sdbusplus::exception::SdBusError(
            -ENOTCONN, "No diagnostic session in progress");
    }

    StateType decoded;
    if (state == statePostCodeReceived)
    {
        decoded = StateType::PostCodeReceived;
    }
    else if (state == stateSystemConfigRequested)
    {
        decoded = StateType::SystemConfigRequested;
    }
    else if (state == stateTIDConfigRequested)
    {
        decoded = StateType::TIDConfigRequested;
    }
    else if (state == stateHeartbeatReceived)
    {
        decoded = StateType::HeartbeatReceived;
    }
    else if (state == stateResultReceived)
    {
        decoded = StateType::ResultReceived;
    }
    else if (state == stateSessionEnded)
    {
        decoded = StateType::SessionEnded;
    }
    else
    {
        lg2::error("PreBootDiag: Unknown State value {S}", "S", state);
        throw sdbusplus::exception::SdBusError(-EINVAL, "Unknown State value");
    }

    switch (decoded)
    {
        case StateType::PostCodeReceived:
        {
            nlohmann::json payloadJson;
            try
            {
                payloadJson = nlohmann::json::parse(payload);
            }
            catch (const nlohmann::json::exception& e)
            {
                lg2::error(
                    "PreBootDiag: PostCodeReceived payload invalid JSON: "
                    "{ERR}",
                    "ERR", e.what());
                throw sdbusplus::exception::SdBusError(
                    -EINVAL, "PostCodeReceived payload is not valid JSON");
            }
            if (!payloadJson.contains("PostCodeName") ||
                !payloadJson["PostCodeName"].is_string())
            {
                lg2::error("PreBootDiag: PostCodeReceived payload missing "
                           "PostCodeName");
                throw sdbusplus::exception::SdBusError(
                    -EINVAL, "PostCodeReceived payload missing PostCodeName");
            }
            auto name = payloadJson["PostCodeName"].get<std::string>();

            if (name == postCodePscFmcBootModePrebootDiag)
            {
                if (pscFmcPostCodeReceived)
                {
                    lg2::info("PreBootDiag: {N} already signaled, ignoring",
                              "N", name);
                    break;
                }
                pscFmcPostCodeReceived = true;
                lg2::info("PreBootDiag: PostCode {N} received", "N", name);
                // Wake waitForPostCodes so it advances to the MB2 phase.
                if (gateWaitTimer)
                {
                    gateWaitTimer->cancel();
                }
            }
            else if (name == postCodeMb2CcplexPrebootDiagEntry)
            {
                if (!pscFmcPostCodeReceived)
                {
                    lg2::error("PreBootDiag: {N} received before {P}, aborting "
                               "session",
                               "N", name, "P",
                               postCodePscFmcBootModePrebootDiag);
                    abortReason = std::string("Out-of-order post code: ") +
                                  name + " received before " +
                                  postCodePscFmcBootModePrebootDiag;
                    abortRequested = true;
                    if (gateWaitTimer)
                    {
                        gateWaitTimer->cancel();
                    }
                    throw sdbusplus::exception::SdBusError(
                        -EPROTO, "Post code received out of order");
                }
                if (mb2PostCodeReceived)
                {
                    lg2::info("PreBootDiag: {N} already signaled, ignoring",
                              "N", name);
                    break;
                }
                mb2PostCodeReceived = true;
                lg2::info("PreBootDiag: PostCode {N} received", "N", name);
                if (gateWaitTimer)
                {
                    gateWaitTimer->cancel();
                }
            }
            else
            {
                lg2::error("PreBootDiag: Unknown PostCodeName {N}", "N", name);
                throw sdbusplus::exception::SdBusError(-EINVAL,
                                                       "Unknown PostCodeName");
            }
            break;
        }

        // NSM loop states: push into channel for listenForEvents to consume.
        case StateType::SystemConfigRequested:
        case StateType::TIDConfigRequested:
        case StateType::HeartbeatReceived:
        case StateType::ResultReceived:
        case StateType::SessionEnded:
            if (!eventChannel)
            {
                lg2::warning(
                    "PreBootDiag: NSM state {S} received before event loop, "
                    "dropping",
                    "S", state);
                return;
            }
            if (!eventChannel->try_send(boost::system::error_code{},
                                        EventEntry{decoded, payload}))
            {
                lg2::critical(
                    "PreBootDiag: event channel full, aborting session "
                    "(dropped {S})",
                    "S", state);
                abortRequested = true;
                throw sdbusplus::exception::SdBusError(
                    -EOVERFLOW, "Event channel full, session being aborted");
            }
            break;
    }
}

boost::asio::awaitable<void> PreBootDiag::runDiagnosticSession()
{
    std::string failureReason;

    try
    {
        auto status = co_await dbus->getDiagStatus();
        if (status == DiagStatus::TestRunning ||
            status == DiagStatus::InProgress)
        {
            lg2::error(
                "PreBootDiag: DiagStatus is {STATUS}, cannot start session",
                "STATUS", lg2::hex, static_cast<uint8_t>(status));
            throw std::runtime_error("Diagnostic test already running");
        }

        if (!co_await dbus->hasDiagConfig())
        {
            lg2::error("PreBootDiag: No DiagConfig in Settings, aborting");
            throw std::runtime_error("No DiagConfig present in Settings");
        }

        co_await dbus->setDiagStatus(DiagStatus::InProgress);
        co_await dbus->setDiagMode(true);
        co_await dbus->setSettingsStringProperty("DiagResult", "[]");
        lg2::info("PreBootDiag: Session started, DiagStatus=InProgress");

        initGpioSequence();

        co_await waitForPostCodes();

        co_await listenForEvents();

        recoverCpus();
    }
    catch (const std::exception& e)
    {
        failureReason = e.what();
        lg2::error("PreBootDiag: Session failed: {ERR}", "ERR", failureReason);
    }

    if (!failureReason.empty())
    {
        clearBootChainGpios();

        try
        {
            co_await dbus->setDiagStatus(DiagStatus::Abort);
            co_await dbus->setDiagMode(false);
        }
        catch (const std::exception& se)
        {
            lg2::error("PreBootDiag: Failed to set abort status: {ERR}", "ERR",
                       se.what());
        }

        try
        {
            co_await dbus->createErrorLog(
                "PreBootDiag diagnostic failed: " + failureReason, "");
        }
        catch (const std::exception& le)
        {
            lg2::error("PreBootDiag: Failed to create error log: {ERR}", "ERR",
                       le.what());
        }
    }

    sequenceRunning = false;
    eventChannel.reset();

    /// Reset the Enabled property so the next setEnabled(true) is not
    /// short-circuited by setEnabled's `requested == enabled` guard. This
    /// makes re-enabling after a failure a single property write, no
    /// disable/enable toggle required.
    if (enabled)
    {
        enabled = false;
        enableIface->set_property("Enabled", false);
    }
}

void PreBootDiag::initGpioSequence()
{
    // Phase 1: assert reset on every board before touching any straps.
    if (auto ec = gpio->setPins({istSysRstBrd0Gpio, istSysRstBrd1Gpio}, 0))
    {
        throw std::runtime_error(std::string("Failed to assert reset: ") +
                                 ec.message());
    }

    // Phase 2: program boot-chain straps and HOLD them. The strap must
    // remain driven by prebootdiag for the entire session — any reset the
    // CPUs see (now or later, before clearBootChainGpios) must sample
    // strap=1 at BootROM time. Released lines revert to kernel defaults.
    if (auto ec =
            gpio->holdPins({cpuBootChain0Brd0Gpio, cpuBootChain0Brd1Gpio}, 1))
    {
        throw std::runtime_error(
            std::string("Failed to hold boot-chain strap: ") + ec.message());
    }

    // Phase 3: release reset; CPUs sample the new straps and enter
    // preboot-diag boot.
    if (auto ec = gpio->setPins({istSysRstBrd0Gpio, istSysRstBrd1Gpio}, 1))
    {
        throw std::runtime_error(std::string("Failed to release reset: ") +
                                 ec.message());
    }
}

void PreBootDiag::clearBootChainGpios()
{
    // Best-effort cleanup: per-pin success / failure is already logged
    // inside setPin / holdPin; we discard the aggregate return so a single
    // failed pin doesn't skip the rest of the recovery sequence.

    // Phase 1: drive the still-held boot-chain straps low to confirm a
    // cleared sample, then release them so normal boot sees the kernel
    // default. Strap must transition to 0 before any reset edge so an
    // early/glitched BootROM sample can't re-latch diag mode.
    gpio->holdPins({cpuBootChain0Brd0Gpio, cpuBootChain0Brd1Gpio}, 0);
    gpio->releasePins({cpuBootChain0Brd0Gpio, cpuBootChain0Brd1Gpio});

    // Phase 2: pulse reset asserted to stop the running diag firmware.
    // Transient: line is released back to the kernel after the write so
    // prebootdiag does not retain ownership of IST_SYS_RST.
    gpio->setPins({istSysRstBrd0Gpio, istSysRstBrd1Gpio}, 0);

    // Phase 3: pulse reset deasserted; CPUs sample the cleared straps
    // and re-enter normal boot. Transient as well.
    gpio->setPins({istSysRstBrd0Gpio, istSysRstBrd1Gpio}, 1);
}

boost::asio::awaitable<void> PreBootDiag::waitForPostCodes()
{
    if (pscFmcPostCodeReceived && mb2PostCodeReceived)
    {
        lg2::info("PreBootDiag: Both post codes already received, "
                  "skipping wait");
        co_return;
    }

    auto executor = co_await boost::asio::this_coro::executor;

    auto waitForPostCode = [&](const char* name, bool& received,
                               std::chrono::milliseconds duration)
        -> boost::asio::awaitable<void> {
        if (received)
        {
            co_return;
        }

        gateWaitTimer = std::make_unique<boost::asio::steady_timer>(executor);
        gateWaitTimer->expires_after(duration);

        lg2::info(
            "PreBootDiag: Waiting for post code {N} (timeout {T}s)", "N", name,
            "T",
            std::chrono::duration_cast<std::chrono::seconds>(duration).count());

        try
        {
            co_await gateWaitTimer->async_wait(boost::asio::use_awaitable);
        }
        catch (const boost::system::system_error&)
        {
            /// @note Expected: updateState or Disable cancelled the timer.
        }

        gateWaitTimer.reset();

        if (abortRequested)
        {
            throw std::runtime_error(abortReason.empty()
                                         ? "Diagnostic session aborted by user"
                                         : abortReason);
        }

        if (!received)
        {
            throw std::runtime_error(std::string("Timed out waiting for ") +
                                     name);
        }
    };

    // Phase 1: PSC-FMC (short window, ~30s).
    co_await waitForPostCode(postCodePscFmcBootModePrebootDiag,
                             pscFmcPostCodeReceived, pscFmcWaitDuration);

    // Phase 2: MB2 (long window, ~15min — covers CPU mem training).
    co_await waitForPostCode(postCodeMb2CcplexPrebootDiagEntry,
                             mb2PostCodeReceived, mb2WaitDuration);
}

boost::asio::awaitable<void> PreBootDiag::listenForEvents()
{
    /// Event-idle watchdog: every NSM event (config request, heartbeat,
    /// result, session end) resets the timer. If no event arrives within
    /// `eventTimeout`, the session is presumed stuck and aborted.
    while (true)
    {
        using namespace boost::asio::experimental::awaitable_operators;
        auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer timer(executor, eventTimeout);

        auto result =
            co_await (eventChannel->async_receive(boost::asio::use_awaitable) ||
                      timer.async_wait(boost::asio::use_awaitable));

        if (result.index() == 1)
        {
            throw std::runtime_error(
                "No NSM events received within event-idle timeout");
        }

        EventEntry event = std::get<0>(result);

        if (abortRequested)
        {
            throw std::runtime_error("Diagnostic session aborted by user");
        }

        switch (event.state)
        {
            case StateType::SystemConfigRequested:
                lg2::info("PreBootDiag: NSM state -> SystemConfigRequested");
                co_await handleSystemConfigRequested(event.payload);
                break;
            case StateType::TIDConfigRequested:
                lg2::info("PreBootDiag: NSM state -> TIDConfigRequested");
                co_await handleTIDConfigRequested(event.payload);
                break;
            case StateType::HeartbeatReceived:
                lg2::info("PreBootDiag: NSM state -> HeartbeatReceived");
                co_await dbus->setDiagStatus(DiagStatus::TestRunning);
                break;
            case StateType::ResultReceived:
                lg2::info("PreBootDiag: NSM state -> ResultReceived");
                co_await handleResultReceived(event.payload);
                break;
            case StateType::SessionEnded:
                co_await dbus->setDiagStatus(DiagStatus::NotStarted);
                co_await dbus->setDiagMode(false);
                lg2::info("PreBootDiag: Diagnostic session ended");
                co_return;
            default:
                lg2::warning("PreBootDiag: Ignoring non-event state in loop");
                break;
        }
    }
}

boost::asio::awaitable<void>
    PreBootDiag::handleSystemConfigRequested(const std::string& payload)
{
    nlohmann::json payloadJson;
    try
    {
        payloadJson = nlohmann::json::parse(payload);
    }
    catch (const nlohmann::json::exception& e)
    {
        throw std::runtime_error(
            "SystemConfigRequested payload invalid JSON: " +
            std::string(e.what()));
    }

    if (!payloadJson.contains("Eid"))
    {
        throw std::runtime_error(
            "SystemConfigRequested payload missing Eid field");
    }
    uint8_t eid = payloadJson["Eid"].get<uint8_t>();

    auto systemConfig =
        co_await dbus->getSettingsStringProperty("DiagSystemConfig");
    bool ok = co_await dbus->callNsmConfigSetSystem(eid, systemConfig);
    if (!ok)
    {
        std::string msg =
            "SystemConfig push failed for EID=" + std::to_string(eid);
        lg2::error("PreBootDiag: {MSG}", "MSG", msg);
        co_await dbus->createErrorLog("PreBootDiag: " + msg,
                                      std::to_string(eid));
        throw std::runtime_error(msg);
    }
    lg2::info("PreBootDiag: SystemConfig sent to nsmd EID={EID}", "EID", eid);
}

boost::asio::awaitable<void>
    PreBootDiag::handleTIDConfigRequested(const std::string& payload)
{
    nlohmann::json payloadJson;
    try
    {
        payloadJson = nlohmann::json::parse(payload);
    }
    catch (const nlohmann::json::exception& e)
    {
        throw std::runtime_error("TIDConfigRequested payload invalid JSON: " +
                                 std::string(e.what()));
    }

    if (!payloadJson.contains("Tid"))
    {
        throw std::runtime_error(
            "TIDConfigRequested payload missing Tid field");
    }
    if (!payloadJson.contains("Eid"))
    {
        throw std::runtime_error(
            "TIDConfigRequested payload missing Eid field");
    }

    uint8_t requestedTid = payloadJson["Tid"].get<uint8_t>();
    uint8_t eid = payloadJson["Eid"].get<uint8_t>();
    auto allConfigs = co_await dbus->getSettingsStringProperty("DiagConfig");

    nlohmann::json configArray;
    try
    {
        configArray = nlohmann::json::parse(allConfigs);
    }
    catch (const nlohmann::json::exception& e)
    {
        throw std::runtime_error("DiagConfig invalid JSON: " +
                                 std::string(e.what()));
    }

    // NSM Type 4 TID config is wire-defined as an array of entries — keep
    // the array shape for spec compliance even though we currently send at
    // most one TID per request.
    nlohmann::json matched = nlohmann::json::array();
    for (const auto& entry : configArray)
    {
        if (entry.contains("Tid") &&
            entry["Tid"].get<uint8_t>() == requestedTid)
        {
            matched.push_back(entry);
            break;
        }
    }

    if (matched.empty())
    {
        std::string msg = "No DiagConfig found for requested TID=" +
                          std::to_string(requestedTid);
        lg2::error("PreBootDiag: {MSG} (DiagConfig has {N} entries)", "MSG",
                   msg, "N", configArray.size());
        co_await dbus->createErrorLog("PreBootDiag: " + msg, "");
        throw std::runtime_error(msg);
    }

    bool ok = co_await dbus->callNsmConfigSetTID(eid, matched.dump());
    if (!ok)
    {
        std::string msg =
            "TIDConfig push failed for TID=" + std::to_string(requestedTid) +
            " EID=" + std::to_string(eid);
        lg2::error("PreBootDiag: {MSG}", "MSG", msg);
        co_await dbus->createErrorLog(
            "PreBootDiag: " + msg,
            std::to_string(eid) + ":TID=" + std::to_string(requestedTid));
        throw std::runtime_error(msg);
    }
    lg2::info("PreBootDiag: TIDConfig for TID={TID} sent to nsmd EID={EID}",
              "TID", lg2::hex, requestedTid, "EID", eid);
}

boost::asio::awaitable<void>
    PreBootDiag::handleResultReceived(const std::string& payload)
{
    if (payload.empty())
    {
        lg2::info("PreBootDiag: ResultReceived with empty payload, ignoring");
        co_return;
    }

    nlohmann::json newResult;
    try
    {
        newResult = nlohmann::json::parse(payload);
    }
    catch (const nlohmann::json::exception& e)
    {
        throw std::runtime_error("ResultReceived payload invalid JSON: " +
                                 std::string(e.what()));
    }

    if (!newResult.contains("Tid") || !newResult.contains("Result"))
    {
        throw std::runtime_error(
            "ResultReceived payload missing Tid or Result field");
    }

    nlohmann::json resultsArray = nlohmann::json::array();
    try
    {
        auto existing = co_await dbus->getSettingsStringProperty("DiagResult");
        if (!existing.empty())
        {
            auto parsed = nlohmann::json::parse(existing);
            if (parsed.is_array())
            {
                resultsArray = std::move(parsed);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "PreBootDiag: Failed to read existing DiagResult, starting fresh: "
            "{ERR}",
            "ERR", e.what());
    }

    resultsArray.push_back(newResult);
    co_await dbus->setSettingsStringProperty("DiagResult", resultsArray.dump());
    co_await dbus->setDiagStatus(DiagStatus::Completed);
    lg2::info("PreBootDiag: Result appended for TID={TID}, total={N}", "TID",
              lg2::hex, newResult.value("Tid", static_cast<uint8_t>(0)), "N",
              resultsArray.size());
    co_return;
}

void PreBootDiag::recoverCpus()
{
    clearBootChainGpios();
}

} // namespace nvidia::prebootdiag
