/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gpio_monitor_client.hpp"

#include "gpio_property_logic.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <sdbusplus/slot.hpp>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <cstring>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace pwrseq
{
namespace
{

constexpr const char* kGpioService = "xyz.openbmc_project.Gpio.Monitor";
constexpr const char* kGpioRootPath = "/xyz/openbmc_project/gpio/monitor";
constexpr const char* kLineIface = "xyz.openbmc_project.Gpio.Monitor.Line";
constexpr const char* kObjectManagerIface =
    "org.freedesktop.DBus.ObjectManager";

// Match nvidia-code-mgmt/src/dbusutils.hpp PropertyMap so GetManagedObjects /
// PropertiesChanged unpack with this SDK's sdbusplus specializations.
using PropValue =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;
using PropertyMap = std::map<std::string, PropValue>;
using InterfaceMap = std::map<std::string, PropertyMap>;
using ObjectValueTree =
    std::map<sdbusplus::message::object_path, InterfaceMap>;

void addUniqueLine(std::unordered_set<std::string>& lines,
                   const config::LineRef& ref)
{
    if (!ref.line.empty())
    {
        lines.insert(ref.line);
    }
}

// Only PGOOD and BOOT_COMPLETE drive the power-sequence FSM. Boot-status bit
// lines are deliberately excluded: Redfish assembles BootStatusCode straight
// from gpio-mon, so subscribing to those edges here would be pure overhead.
std::unordered_set<std::string>
    inputLines(const config::PlatformDescriptor& desc)
{
    std::unordered_set<std::string> lines;

    for (const auto& mod : desc.modules)
    {
        for (const auto& el : mod.elements)
        {
            addUniqueLine(lines, el.pg);
            addUniqueLine(lines, el.bc);
        }
    }
    for (const auto& fpga : desc.fpgas)
    {
        addUniqueLine(lines, fpga.pg);
        addUniqueLine(lines, fpga.bc);
    }

    return lines;
}

std::optional<std::string> asString(const PropValue& v)
{
    if (const auto* s = std::get_if<std::string>(&v))
    {
        return *s;
    }
    return std::nullopt;
}

std::optional<bool> asBool(const PropValue& v)
{
    if (const auto* b = std::get_if<bool>(&v))
    {
        return *b;
    }
    return std::nullopt;
}

std::optional<std::string> lookupString(const PropertyMap& props,
                                        const char* key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    return asString(it->second);
}

std::optional<bool> lookupBool(const PropertyMap& props, const char* key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    return asBool(it->second);
}

} // namespace

struct GpioPropertySubscriber::Impl
{
    Impl(sdbusplus::bus::bus& bus, DBusManager& manager,
         std::unordered_set<std::string> interesting) :
        bus_(bus), manager_(manager), interesting_(std::move(interesting))
    {
        // Matches BEFORE any GetManagedObjects — with *async* snapshot the
        // FIFO order of sd_bus_process is preserved (unlike sync sd_bus_call,
        // which queues match signals until after the reply and can replay
        // stale Value deltas). See gpio_property_logic.hpp header comment.
        namespace rules = sdbusplus::bus::match::rules;

        const auto propsRule =
            rules::type::signal() + rules::sender(kGpioService) +
            rules::path_namespace(kGpioRootPath) +
            rules::member("PropertiesChanged") +
            rules::interface("org.freedesktop.DBus.Properties") +
            rules::argN(0, kLineIface);

        propsMatch_ = std::make_unique<sdbusplus::bus::match_t>(
            bus_, propsRule,
            [this](sdbusplus::message_t& msg) { onPropertiesChanged(msg); });

        ifacesAddedMatch_ = std::make_unique<sdbusplus::bus::match_t>(
            bus_,
            rules::interfacesAdded(kGpioRootPath) + rules::sender(kGpioService),
            [this](sdbusplus::message_t& msg) { onInterfacesAdded(msg); });

        ifacesRemovedMatch_ = std::make_unique<sdbusplus::bus::match_t>(
            bus_,
            rules::interfacesRemoved(kGpioRootPath) +
                rules::sender(kGpioService),
            [this](sdbusplus::message_t& msg) { onInterfacesRemoved(msg); });

        nameOwnerMatch_ = std::make_unique<sdbusplus::bus::match_t>(
            bus_, rules::nameOwnerChanged(kGpioService),
            [this](sdbusplus::message_t& msg) { onNameOwnerChanged(msg); });

        applyMachine(gpio_props::SnapshotEvent::StartRequested);
    }

    ~Impl()
    {
        cancelRetryTimer();
        snapshotSlot_ = sdbusplus::slot_t{};
    }

    void applyMachine(gpio_props::SnapshotEvent ev)
    {
        auto actions = machine_.onEvent(ev);
        if (actions.clearPathMap)
        {
            byPath_.clear();
        }
        if (actions.cancelRetryTimer)
        {
            cancelRetryTimer();
        }
        if (actions.armRetryTimer)
        {
            armRetryTimer(actions.retryDelayUsec);
        }
        if (actions.dispatchCall)
        {
            dispatchSnapshotCall();
        }
    }

    void handleDecision(const gpio_props::Decision& d, const char* source)
    {
        if (d.action != gpio_props::Action::Inject)
        {
            return;
        }
        manager_.injectLineState(d.lineName, d.asserted, source);
    }

    void applyLineProps(const std::string& path, const PropertyMap& props,
                        const char* source, bool fromSnapshot)
    {
        auto& cache = byPath_[path];
        gpio_props::Decision decision;
        if (fromSnapshot)
        {
            decision = gpio_props::applySnapshotUpdate(
                cache, lookupString(props, "LineName"),
                lookupBool(props, "Bound"), lookupBool(props, "Value"),
                interesting_);
        }
        else
        {
            decision = gpio_props::applyPropertyUpdate(
                cache, lookupString(props, "LineName"),
                lookupBool(props, "Bound"), lookupBool(props, "Value"),
                interesting_);
        }
        handleDecision(decision, source);
    }

    void dispatchSnapshotCall()
    {
        try
        {
            auto method = bus_.new_method_call(
                kGpioService, kGpioRootPath, kObjectManagerIface,
                "GetManagedObjects");
            // Keep the slot so the async call stays alive until the reply.
            snapshotSlot_ = method.call_async(
                [this](sdbusplus::message_t reply) { onSnapshotReply(reply); });
            machine_.onEvent(gpio_props::SnapshotEvent::CallDispatched);
            lg2::debug("GpioPropertySubscriber: async GetManagedObjects "
                       "dispatched (attempt={N})",
                       "N", machine_.attempts);
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            const char* errName = e.name();
            if (errName != nullptr &&
                gpio_props::isServiceMissingError(errName))
            {
                lg2::info("GpioPropertySubscriber: GetManagedObjects not "
                          "started ({ERR}); waiting for {SVC}",
                          "ERR", e.what(), "SVC", std::string{kGpioService});
                applyMachine(gpio_props::SnapshotEvent::CallFailedMissing);
            }
            else
            {
                lg2::warning("GpioPropertySubscriber: GetManagedObjects "
                             "dispatch failed ({ERR}); scheduling retry",
                             "ERR", e.what());
                applyMachine(gpio_props::SnapshotEvent::CallFailedTransient);
            }
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: GetManagedObjects "
                         "dispatch failed ({ERR}); scheduling retry",
                         "ERR", e.what());
            applyMachine(gpio_props::SnapshotEvent::CallFailedTransient);
        }
    }

    void onSnapshotReply(sdbusplus::message_t& reply)
    {
        // Drop the slot handle after the callback; destroy callback runs
        // after we return.
        snapshotSlot_ = sdbusplus::slot_t{};

        if (reply.is_method_error())
        {
            const sd_bus_error* err = reply.get_error();
            const char* errName =
                (err != nullptr && err->name != nullptr) ? err->name : "";
            const char* errMsg =
                (err != nullptr && err->message != nullptr) ? err->message
                                                             : "method error";
            if (gpio_props::isServiceMissingError(errName))
            {
                lg2::info("GpioPropertySubscriber: GetManagedObjects "
                          "missing service ({NAME}: {ERR}); waiting for name",
                          "NAME", errName, "ERR", errMsg);
                applyMachine(gpio_props::SnapshotEvent::CallFailedMissing);
                return;
            }
            lg2::warning("GpioPropertySubscriber: GetManagedObjects "
                         "failed ({NAME}: {ERR}); retry if bounded",
                         "NAME", errName, "ERR", errMsg);
            applyMachine(gpio_props::SnapshotEvent::CallFailedTransient);
            return;
        }

        try
        {
            ObjectValueTree tree;
            reply.read(tree);

            uint32_t seen = 0;
            uint32_t injected = 0;
            for (const auto& [objPath, ifaces] : tree)
            {
                auto it = ifaces.find(kLineIface);
                if (it == ifaces.end())
                {
                    continue;
                }
                ++seen;
                const std::string path = static_cast<std::string>(objPath);
                auto& cache = byPath_[path];
                auto decision = gpio_props::applySnapshotUpdate(
                    cache, lookupString(it->second, "LineName"),
                    lookupBool(it->second, "Bound"),
                    lookupBool(it->second, "Value"), interesting_);
                if (decision.action == gpio_props::Action::Inject)
                {
                    ++injected;
                }
                handleDecision(decision, "GpioPropertySnapshot");
            }

            applyMachine(gpio_props::SnapshotEvent::CallSucceeded);
            lg2::info("GpioPropertySubscriber: snapshot complete "
                      "(lineObjects={SEEN}, injected={INJ}, "
                      "interesting={INT})",
                      "SEEN", seen, "INJ", injected, "INT",
                      static_cast<uint32_t>(interesting_.size()));
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: snapshot parse failed "
                         "({ERR}); retry if bounded",
                         "ERR", e.what());
            applyMachine(gpio_props::SnapshotEvent::CallFailedTransient);
        }
    }

    void onPropertiesChanged(sdbusplus::message_t& msg)
    {
        try
        {
            std::string iface;
            PropertyMap changed;
            std::vector<std::string> invalidated;
            msg.read(iface, changed, invalidated);
            if (iface != kLineIface)
            {
                return;
            }
            (void)invalidated;
            applyLineProps(msg.get_path(), changed, "GpioPropertyChanged",
                           /*fromSnapshot=*/false);
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: PropertiesChanged parse "
                         "failed: {ERR}",
                         "ERR", e.what());
        }
    }

    void onInterfacesAdded(sdbusplus::message_t& msg)
    {
        try
        {
            sdbusplus::message::object_path path;
            InterfaceMap ifaces;
            msg.read(path, ifaces);
            auto it = ifaces.find(kLineIface);
            if (it == ifaces.end())
            {
                return;
            }
            applyLineProps(static_cast<std::string>(path), it->second,
                           "GpioInterfacesAdded", /*fromSnapshot=*/false);
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: InterfacesAdded parse "
                         "failed: {ERR}",
                         "ERR", e.what());
        }
    }

    void onInterfacesRemoved(sdbusplus::message_t& msg)
    {
        try
        {
            sdbusplus::message::object_path path;
            std::vector<std::string> ifaces;
            msg.read(path, ifaces);
            for (const auto& iface : ifaces)
            {
                if (iface == kLineIface)
                {
                    byPath_.erase(static_cast<std::string>(path));
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: InterfacesRemoved parse "
                         "failed: {ERR}",
                         "ERR", e.what());
        }
    }

    void onNameOwnerChanged(sdbusplus::message_t& msg)
    {
        try
        {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg.read(name, oldOwner, newOwner);
            if (name != kGpioService)
            {
                return;
            }
            if (newOwner.empty())
            {
                lg2::info("GpioPropertySubscriber: {SVC} left the bus",
                          "SVC", std::string{kGpioService});
                snapshotSlot_ = sdbusplus::slot_t{};
                applyMachine(gpio_props::SnapshotEvent::ServiceDeparted);
            }
            else if (oldOwner.empty())
            {
                lg2::info("GpioPropertySubscriber: {SVC} appeared; "
                          "async re-snapshot",
                          "SVC", std::string{kGpioService});
                applyMachine(gpio_props::SnapshotEvent::ServiceAppeared);
            }
        }
        catch (const std::exception& e)
        {
            lg2::warning("GpioPropertySubscriber: NameOwnerChanged parse "
                         "failed: {ERR}",
                         "ERR", e.what());
        }
    }

    void cancelRetryTimer()
    {
        if (retrySource_ != nullptr)
        {
            sd_event_source_set_enabled(retrySource_, SD_EVENT_OFF);
            sd_event_source_unref(retrySource_);
            retrySource_ = nullptr;
        }
    }

    void armRetryTimer(uint64_t delayUsec)
    {
        cancelRetryTimer();
        sd_bus* raw = sdbusplus::bus::details::bus_friend::get_busp(bus_);
        sd_event* loop = sd_bus_get_event(raw);
        if (loop == nullptr)
        {
            lg2::error("GpioPropertySubscriber: no sd_event on bus; cannot "
                       "arm snapshot retry");
            return;
        }

        uint64_t now = 0;
        int rc = sd_event_now(loop, CLOCK_MONOTONIC, &now);
        if (rc < 0)
        {
            lg2::error("GpioPropertySubscriber: sd_event_now failed: {ERR}",
                       "ERR", std::strerror(-rc));
            return;
        }

        rc = sd_event_add_time(loop, &retrySource_, CLOCK_MONOTONIC,
                               now + delayUsec, 0, &Impl::retryTimerCb, this);
        if (rc < 0)
        {
            retrySource_ = nullptr;
            lg2::error("GpioPropertySubscriber: sd_event_add_time failed: "
                       "{ERR}",
                       "ERR", std::strerror(-rc));
            return;
        }
        lg2::info("GpioPropertySubscriber: retry GetManagedObjects in "
                  "{US}us (attempt={N}/{MAX})",
                  "US", delayUsec, "N", machine_.attempts, "MAX",
                  machine_.maxAttempts);
    }

    static int retryTimerCb(sd_event_source* /*s*/, uint64_t /*usec*/,
                            void* userdata)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (self->retrySource_ != nullptr)
        {
            sd_event_source_unref(self->retrySource_);
            self->retrySource_ = nullptr;
        }
        self->applyMachine(gpio_props::SnapshotEvent::RetryTimerFired);
        return 0;
    }

    sdbusplus::bus::bus& bus_;
    DBusManager& manager_;
    std::unordered_set<std::string> interesting_;
    std::unordered_map<std::string, gpio_props::CachedLine> byPath_;
    gpio_props::SnapshotMachine machine_{};

    std::unique_ptr<sdbusplus::bus::match_t> propsMatch_;
    std::unique_ptr<sdbusplus::bus::match_t> ifacesAddedMatch_;
    std::unique_ptr<sdbusplus::bus::match_t> ifacesRemovedMatch_;
    std::unique_ptr<sdbusplus::bus::match_t> nameOwnerMatch_;

    sdbusplus::slot_t snapshotSlot_{};
    sd_event_source* retrySource_ = nullptr;
};

GpioPropertySubscriber::GpioPropertySubscriber(
    sdbusplus::bus::bus& bus, const config::PlatformDescriptor& desc,
    DBusManager& manager)
{
    auto interesting = inputLines(desc);
    lg2::info("GpioPropertySubscriber: starting (interestingLines={N})", "N",
              static_cast<uint32_t>(interesting.size()));
    impl_ = std::make_unique<Impl>(bus, manager, std::move(interesting));
}

GpioPropertySubscriber::~GpioPropertySubscriber() = default;

GpioPropertySubscriber::GpioPropertySubscriber(
    GpioPropertySubscriber&&) noexcept = default;
GpioPropertySubscriber& GpioPropertySubscriber::operator=(
    GpioPropertySubscriber&&) noexcept = default;

} // namespace pwrseq
