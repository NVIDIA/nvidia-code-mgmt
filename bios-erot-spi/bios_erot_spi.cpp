// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "constants.hpp"
#include "dbusutils.hpp"
#include "handler.hpp"
#include "instance_id.hpp"
#include "socket_handler.hpp"
#include "socket_manager.hpp"
#include "types.hpp"

#include <endian.h>
#include <sys/socket.h>

#include <com/nvidia/BIOS/ClearNVRAM/server.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Common/Progress/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using ClearNVRAMInterface = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::BIOS::server::ClearNVRAM>;
using ProgressInterface = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::common::Progress>;

constexpr auto serviceName = "com.Nvidia.BiosEroTSpi";
constexpr auto objectManagerPath = "/xyz/openbmc_project/bios";
constexpr auto clearNVRAMPath = "/xyz/openbmc_project/bios/clear_nvram";
constexpr auto clearProgressPath =
    "/xyz/openbmc_project/status/BIOS_ClearNVRAM";
constexpr auto inventoryRootPath = "/xyz/openbmc_project/inventory";
constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto configInterface =
    "xyz.openbmc_project.Configuration.BIOSClearNVRAM";

constexpr uint8_t spiRegionCommand = 0x0E;
constexpr uint8_t queryAction = 0x01;
constexpr uint8_t clearAction = 0x02;
constexpr uint8_t immediateClearMode = 0x01;
// ERoT ignores Param#1 for Query. Preserve the hardware-validated value.
constexpr uint8_t ignoredQueryParameter = 0x01;
constexpr uint8_t completionSuccess = 0x00;
constexpr uint8_t copySuccess = 0x01;
constexpr uint8_t copySent = 0x02;
constexpr uint8_t querySuccess = 0x01;
constexpr uint8_t queryInProgress = 0x03;
constexpr size_t spiResponseSize = 2;
constexpr size_t maxProgressHistory = 3;
constexpr std::size_t maxStatusQueries = 30;
constexpr auto initialRequestDelay = std::chrono::milliseconds(10);
constexpr auto statusQueryInterval = std::chrono::seconds(1);

struct VdmCommand
{
    uint8_t code;
    std::span<const uint8_t> payload;
    size_t responseSize;
    uint8_t version = mctp_vdm::nvidiaMsgVersion;
};

struct OperationConfig
{
    const char* progressPath;
    VdmCommand request;
    std::span<const uint8_t> acceptedRequestStatuses;
    uint8_t initialProgress;
    VdmCommand query;
    uint8_t completeStatus;
    uint8_t inProgressStatus;
};

class BiosEroTSpi : public ClearNVRAMInterface
{
  public:
    /**
     * @brief Construct a BIOS NVRAM clear provider.
     *
     * @param bus D-Bus connection used to publish the provider and progress.
     * @param event Event loop used by operation timers and MCTP requests.
     * @param requestHandler NVIDIA VDM request handler.
     * @param socketHandler Kernel MCTP socket handler.
     * @param instanceIdManager NVIDIA VDM instance ID manager.
     * @param eid MCTP endpoint ID of the SBIOS ERoT.
     */
    BiosEroTSpi(sdbusplus::bus::bus& bus, sdeventplus::Event& event,
                mctp_vdm::requester::Handler& requestHandler,
                mctp_socket::Handler& socketHandler,
                mctp_vdm::InstanceIdMgr& instanceIdManager, uint8_t eid) :
        ClearNVRAMInterface(bus, clearNVRAMPath,
                            ClearNVRAMInterface::action::emit_interface_added),
        bus(bus), requestHandler(requestHandler), socketHandler(socketHandler),
        instanceIdManager(instanceIdManager), eid(eid),
        operationTimer(event.get())
    {
        socketHandler.registerMctpEndpoint(eid, SOCK_DGRAM, 0, {});
        lg2::info("Created BIOS ClearNVRAM provider for EID={EID}", "EID", eid);
    }

    /**
     * @brief Start an asynchronous clear of the active BIOS NVRAM.
     *
     * @return Object path implementing Common.Progress for the operation.
     * @throws Common.Error.Unavailable if another clear is already running.
     */
    sdbusplus::message::object_path clear() override
    {
        return startOperation(Operation::clear);
    }

  private:
    enum class Operation
    {
        clear
    };

    /**
     * @brief Describe each operation's commands and response format.
     *
     * Payloads have static storage so their spans remain valid across
     * coroutine suspension.
     */
    static const OperationConfig& getOperationConfig(Operation operation)
    {
        static constexpr std::array<uint8_t, 3> clearPayload{
            clearAction, immediateClearMode, 0};
        static constexpr std::array<uint8_t, 3> clearQueryPayload{
            queryAction, ignoredQueryParameter, 0};
        static constexpr std::array<uint8_t, 2> clearAcceptedStatuses{
            copySuccess, copySent};
        static constexpr OperationConfig clearConfig{
            .progressPath = clearProgressPath,
            .request = {.code = spiRegionCommand,
                        .payload = clearPayload,
                        .responseSize = spiResponseSize},
            .acceptedRequestStatuses = clearAcceptedStatuses,
            .initialProgress = 30,
            .query = {.code = spiRegionCommand,
                      .payload = clearQueryPayload,
                      .responseSize = spiResponseSize},
            .completeStatus = querySuccess,
            .inProgressStatus = queryInProgress,
        };

        switch (operation)
        {
            case Operation::clear:
                return clearConfig;
        }
        throw std::invalid_argument("Unsupported SBIOS ERoT operation");
    }

    /**
     * @brief Start an asynchronous SBIOS ERoT operation.
     *
     * @param operation Operation to start.
     * @return Object path implementing Common.Progress for the operation.
     */
    sdbusplus::message::object_path startOperation(Operation operation)
    {
        if (operationActive)
        {
            lg2::error("SBIOS ERoT operation unavailable. EID={EID}, busy=true",
                       "EID", eid);
            throw sdbusplus::xyz::openbmc_project::Common::Error::Unavailable();
        }

        const auto& config = getOperationConfig(operation);
        if (progressHistory.size() >= maxProgressHistory)
        {
            progressHistory.pop_front();
        }

        const std::string path = std::string(config.progressPath) + "_" +
                                 std::to_string(operationIndex++);
        auto progress = std::make_unique<ProgressInterface>(bus, path.c_str());
        progress->startTime(nowMicroseconds());
        progress->status(ProgressInterface::OperationStatus::InProgress);
        progress->progress(0);
        progressHistory.push_back(std::move(progress));

        currentProgress = 0;
        operationActive = true;
        try
        {
            runOperation(operation);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to start SBIOS ERoT operation: {ERROR}", "ERROR",
                       e.what());
            finishOperation(false);
        }
        catch (...)
        {
            lg2::error(
                "Failed to start SBIOS ERoT operation: unknown exception");
            finishOperation(false);
        }
        return sdbusplus::message::object_path(path);
    }

    /**
     * @brief Get the current wall-clock time in microseconds.
     *
     * @return Microseconds elapsed since the Unix epoch.
     */
    static uint64_t nowMicroseconds()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @brief Get the progress object for the active operation.
     *
     * @return Active progress object, or nullptr when history is empty.
     */
    ProgressInterface* currentProgressObject()
    {
        return progressHistory.empty() ? nullptr : progressHistory.back().get();
    }

    /**
     * @brief Validate an ERoT response and extract its operation status.
     *
     * @param response Response returned by the MCTP requester.
     * @param responseLength Length of the returned response.
     * @param expectedSize Expected response payload size.
     * @return Response payload, or nullopt for an invalid response.
     */
    static std::optional<std::vector<uint8_t>>
        parseResponse(const mctp_vdm::Message* response, size_t responseLength,
                      size_t expectedSize)
    {
        if (response == nullptr)
        {
            lg2::error("BIOS ERoT VDM response is missing");
            return std::nullopt;
        }
        if (responseLength != expectedSize)
        {
            lg2::error(
                "Unexpected BIOS ERoT VDM response length={LENGTH}, expected={EXPECTED}",
                "LENGTH", responseLength, "EXPECTED", expectedSize);
            return std::nullopt;
        }
        if (response->payload[0] != completionSuccess)
        {
            lg2::error("BIOS ERoT VDM completion code={CODE}", "CODE",
                       response->payload[0]);
            return std::nullopt;
        }
        return std::vector<uint8_t>(response->payload,
                                    response->payload + responseLength);
    }

    /**
     * @brief Eager, self-destroying coroutine for one ERoT operation.
     */
    struct OperationCoroutine
    {
        struct promise_type
        {
            OperationCoroutine get_return_object() noexcept
            {
                return {};
            }

            std::suspend_never initial_suspend() const noexcept
            {
                return {};
            }

            std::suspend_never final_suspend() const noexcept
            {
                return {};
            }

            void return_void() const noexcept
            {}

            [[noreturn]] void unhandled_exception() const noexcept
            {
                std::terminate();
            }
        };
    };

    /**
     * @brief One-shot timer that resumes one suspended coroutine.
     */
    class CoroutineTimer
    {
      public:
        explicit CoroutineTimer(sd_event* event) :
            timer(event, [this]() { resume(); })
        {}

        void suspend(std::coroutine_handle<> handle,
                     std::chrono::microseconds delay)
        {
            if (sleeper)
            {
                throw std::logic_error(
                    "BIOS ClearNVRAM timer already has a waiter");
            }
            timer.start(delay, false);
            sleeper = handle;
        }

        void clearWaiter() noexcept
        {
            sleeper = {};
        }

        bool isRunning() const
        {
            return timer.isRunning();
        }

        int stop()
        {
            return timer.stop();
        }

      private:
        void resume()
        {
            if (auto handle = std::exchange(sleeper, {}))
            {
                handle.resume();
            }
        }

        sdbusplus::Timer timer;
        std::coroutine_handle<> sleeper;
    };

    /**
     * @brief Awaitable backed by the class-owned one-shot operation timer.
     */
    struct Sleep
    {
        template <typename Rep, typename Period>
        Sleep(CoroutineTimer& timer, std::chrono::duration<Rep, Period> delay) :
            timer(timer),
            delay(std::chrono::duration_cast<std::chrono::microseconds>(delay))
        {}

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle)
        {
            timer.suspend(handle, delay);
        }

        void await_resume() const noexcept
        {}

        CoroutineTimer& timer;
        std::chrono::microseconds delay;
    };

    /**
     * @brief Awaitable for one SBIOS ERoT VDM command.
     */
    struct ErotCommand
    {
        ErotCommand(BiosEroTSpi& self, VdmCommand command) :
            self(self), command(command),
            activationRc(self.socketHandler.activateSockets({self.eid})),
            inner(self.requestHandler, self.eid, request, &response,
                  &responseLength)
        {
            if (activationRc < 0)
            {
                lg2::error(
                    "Failed to activate MCTP socket for EID={EID}, RC={RC}",
                    "EID", self.eid, "RC", activationRc);
                return;
            }

            request.resize(sizeof(mctp_vdm::MsgHeader) +
                           command.payload.size());
            auto* requestMessage =
                reinterpret_cast<mctp_vdm::Message*>(request.data());
            requestMessage->hdr.iana = htobe32(mctp_vdm::nvidiaIANA);
            requestMessage->hdr.request = 1;
            requestMessage->hdr.instanceId =
                self.instanceIdManager.getInstanceId(self.eid);
            requestMessage->hdr.msgType = mctp_vdm::nvidiaMsgType;
            requestMessage->hdr.commandCode = command.code;
            requestMessage->hdr.msgVersion = command.version;
            std::copy(command.payload.begin(), command.payload.end(),
                      request.begin() + sizeof(mctp_vdm::MsgHeader));
        }

        ErotCommand(const ErotCommand&) = delete;
        ErotCommand& operator=(const ErotCommand&) = delete;
        ErotCommand(ErotCommand&&) = delete;
        ErotCommand& operator=(ErotCommand&&) = delete;

        bool await_ready() noexcept
        {
            return activationRc < 0 || inner.await_ready();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            return inner.await_suspend(handle);
        }

        std::optional<std::vector<uint8_t>> await_resume()
        {
            if (activationRc < 0)
            {
                return std::nullopt;
            }

            const auto rc = inner.await_resume();
            if (rc != 0)
            {
                lg2::error(
                    "SBIOS ERoT VDM command failed. EID={EID}, COMMAND={COMMAND}, RC={RC}",
                    "EID", self.eid, "COMMAND", command.code, "RC", rc);
                return std::nullopt;
            }
            return BiosEroTSpi::parseResponse(response, responseLength,
                                              command.responseSize);
        }

      private:
        BiosEroTSpi& self;
        VdmCommand command;
        int activationRc;
        mctp::Request request;
        const mctp_vdm::Message* response = nullptr;
        size_t responseLength = 0;
        mctp_vdm::requester::SendRecvMctpVdmMsg inner;
    };

    /**
     * @brief Update the active operation's progress.
     *
     * @param value New progress percentage.
     */
    void setProgress(uint8_t value)
    {
        currentProgress = value;
        if (auto* progress = currentProgressObject())
        {
            progress->progress(currentProgress);
        }
    }

    /**
     * @brief Run one SBIOS ERoT operation from request through polling.
     *
     * @param operation Operation to execute.
     */
    OperationCoroutine runOperation(Operation operation)
    {
        bool success = false;
        try
        {
            const auto& config = getOperationConfig(operation);
            co_await Sleep{operationTimer, initialRequestDelay};
            auto response = co_await ErotCommand{*this, config.request};
            const auto acceptedStatuses = config.acceptedRequestStatuses;
            if (response.has_value() &&
                (acceptedStatuses.empty() ||
                 std::find(acceptedStatuses.begin(), acceptedStatuses.end(),
                           response->at(1)) != acceptedStatuses.end()))
            {
                setProgress(config.initialProgress);
                for (std::size_t query = 0; query < maxStatusQueries; ++query)
                {
                    co_await Sleep{operationTimer, statusQueryInterval};
                    response = co_await ErotCommand{*this, config.query};
                    if (!response.has_value())
                    {
                        break;
                    }
                    const uint8_t status = response->at(1);
                    if (status == config.completeStatus)
                    {
                        success = true;
                        break;
                    }
                    if (status != config.inProgressStatus)
                    {
                        lg2::error(
                            "SBIOS ERoT operation status failed, status={STATUS}",
                            "STATUS", status);
                        break;
                    }
                    if (query + 1 >= maxStatusQueries)
                    {
                        lg2::error(
                            "SBIOS ERoT operation status query timed out");
                        break;
                    }

                    setProgress(std::min<uint8_t>(
                        90, static_cast<uint8_t>(currentProgress + 15)));
                }
            }
            else if (response.has_value())
            {
                lg2::error("SBIOS ERoT rejected operation, command={COMMAND}",
                           "COMMAND", config.request.code);
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("SBIOS ERoT operation failed: {ERROR}", "ERROR",
                       e.what());
        }
        catch (...)
        {
            lg2::error("SBIOS ERoT operation failed: unknown exception");
        }
        finishOperation(success);
    }

    /**
     * @brief Complete the active operation and publish its final status.
     *
     * @param success Whether the ERoT reported successful completion.
     */
    void finishOperation(bool success) noexcept
    {
        operationActive = false;
        operationTimer.clearWaiter();

        try
        {
            if (operationTimer.isRunning())
            {
                const auto rc = operationTimer.stop();
                if (rc)
                {
                    lg2::error(
                        "Failed to stop SBIOS ERoT operation timer. RC={RC}",
                        "RC", rc);
                }
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to stop SBIOS ERoT timer: {ERROR}", "ERROR",
                       e.what());
        }
        catch (...)
        {
            lg2::error("Failed to stop SBIOS ERoT timer: unknown exception");
        }

        try
        {
            socketHandler.deactivateSocket(eid);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to deactivate BIOS ERoT socket: {ERROR}",
                       "ERROR", e.what());
        }
        catch (...)
        {
            lg2::error(
                "Failed to deactivate BIOS ERoT socket: unknown exception");
        }

        auto* progress = currentProgressObject();
        if (progress == nullptr)
        {
            return;
        }

        try
        {
            progress->progress(100);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to finalize SBIOS ERoT Progress: {ERROR}",
                       "ERROR", e.what());
        }
        catch (...)
        {
            lg2::error(
                "Failed to finalize SBIOS ERoT Progress: unknown exception");
        }

        try
        {
            progress->completedTime(nowMicroseconds());
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to set SBIOS ERoT completion time: {ERROR}",
                       "ERROR", e.what());
        }
        catch (...)
        {
            lg2::error(
                "Failed to set SBIOS ERoT completion time: unknown exception");
        }

        try
        {
            progress->status(success
                                 ? ProgressInterface::OperationStatus::Completed
                                 : ProgressInterface::OperationStatus::Failed);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to set SBIOS ERoT status: {ERROR}", "ERROR",
                       e.what());
        }
        catch (...)
        {
            lg2::error("Failed to set SBIOS ERoT status: unknown exception");
        }
    }

    sdbusplus::bus::bus& bus;
    mctp_vdm::requester::Handler& requestHandler;
    mctp_socket::Handler& socketHandler;
    mctp_vdm::InstanceIdMgr& instanceIdManager;
    uint8_t eid;
    uint8_t currentProgress = 0;
    std::size_t operationIndex = 0;
    bool operationActive = false;
    CoroutineTimer operationTimer;
    std::deque<std::unique_ptr<ProgressInterface>> progressHistory;
};

/**
 * @brief Read an unsigned byte property from Entity Manager configuration.
 *
 * @param interfaces Entity Manager interface map.
 * @param property Property name to read.
 * @return Property value, or nullopt when missing, invalid, or out of range.
 */
std::optional<uint8_t> getByteProperty(const dbus::InterfaceMap& interfaces,
                                       const std::string& property)
{
    try
    {
        const auto value =
            std::get<uint64_t>(interfaces.at(configInterface).at(property));
        if (value > UINT8_MAX)
        {
            lg2::error("BIOS ClearNVRAM {PROPERTY} is out of range", "PROPERTY",
                       property);
            return std::nullopt;
        }
        return static_cast<uint8_t>(value);
    }
    catch (const std::out_of_range& e)
    {
        lg2::error("Failed to read BIOS ClearNVRAM {PROPERTY}: {ERROR}",
                   "PROPERTY", property, "ERROR", e.what());
        return std::nullopt;
    }
    catch (const std::bad_variant_access& e)
    {
        lg2::error("Failed to read BIOS ClearNVRAM {PROPERTY}: {ERROR}",
                   "PROPERTY", property, "ERROR", e.what());
        return std::nullopt;
    }
}

/**
 * @brief Create the ClearNVRAM provider from Entity Manager configuration.
 *
 * @param bus D-Bus connection used by the provider.
 * @param event Event loop used by timers and requests.
 * @param requestHandler NVIDIA VDM request handler.
 * @param socketHandler Kernel MCTP socket handler.
 * @param instanceIdManager NVIDIA VDM instance ID manager.
 * @param provider Provider instance to populate.
 */
void populateProvider(sdbusplus::bus::bus& bus, sdeventplus::Event& event,
                      mctp_vdm::requester::Handler& requestHandler,
                      mctp_socket::Handler& socketHandler,
                      mctp_vdm::InstanceIdMgr& instanceIdManager,
                      std::unique_ptr<BiosEroTSpi>& provider)
{
    if (provider)
    {
        return;
    }

    nvidia::software::updater::DBUSUtils dbusUtils(bus);
    const auto objects =
        dbusUtils.getManagedObjects(entityManagerService, inventoryRootPath);
    for (const auto& [unused, interfaces] : objects)
    {
        if (!interfaces.contains(configInterface))
        {
            continue;
        }

        const auto eid = getByteProperty(interfaces, "MctpEID");
        if (!eid.has_value())
        {
            continue;
        }

        provider = std::make_unique<BiosEroTSpi>(
            bus, event, requestHandler, socketHandler, instanceIdManager, *eid);
        return;
    }
}

} // namespace

/**
 * @brief Run the BIOS ERoT SPI NVRAM clear service.
 *
 * @return EXIT_SUCCESS on normal shutdown, otherwise EXIT_FAILURE.
 */
int main()
{
    try
    {
        auto bus = sdbusplus::bus::new_default();
        auto event = sdeventplus::Event::get_default();
        bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
        bus.request_name(serviceName);
        sdbusplus::server::manager_t manager(bus, objectManagerPath);

        mctp_socket::Manager socketManager;
        mctp_vdm::InstanceIdMgr instanceIdManager;
        mctp_vdm::requester::Handler requestHandler(event, instanceIdManager,
                                                    socketManager);
        mctp_socket::Handler socketHandler(event, requestHandler,
                                           socketManager);

        std::unique_ptr<BiosEroTSpi> provider;
        auto inventoryObjectMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            sdbusplus::bus::match::rules::interfacesAdded(inventoryRootPath),
            [&bus, &event, &requestHandler, &socketHandler, &instanceIdManager,
             &provider](sdbusplus::message::message& message) {
                sdbusplus::message::object_path path;
                dbus::InterfaceMap interfaces;
                message.read(path, interfaces);
                if (interfaces.contains(configInterface))
                {
                    populateProvider(bus, event, requestHandler, socketHandler,
                                     instanceIdManager, provider);
                }
            });

        populateProvider(bus, event, requestHandler, socketHandler,
                         instanceIdManager, provider);
        event.loop();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        lg2::error("BiosEroTSpi terminated: {ERROR}", "ERROR", e.what());
        return EXIT_FAILURE;
    }
}
