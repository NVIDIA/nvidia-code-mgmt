/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "dbus_manager.hpp"

#include "hal/i_device.hpp"

#include <phosphor-logging/lg2.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pwrseq
{

namespace
{

constexpr const char* kObjectPath  = "/com/Nvidia/PwrSeqMonitor";
constexpr const char* kInterface   = "com.Nvidia.PwrSeqMonitor.Manager";

// ─── String <-> enum helpers ──────────────────────────────────────────────

std::optional<hal::ResetKind> parseKind(std::string_view s) noexcept
{
    if (s == "Warm")       return hal::ResetKind::Warm;
    if (s == "Cold")       return hal::ResetKind::Cold;
    if (s == "PowerCycle") return hal::ResetKind::PowerCycle;
    return std::nullopt;
}

std::optional<InstanceId::Class> parseElementClass(std::string_view s) noexcept
{
    if (s == "LPU")  return InstanceId::Class::Lpu;
    if (s == "FPGA") return InstanceId::Class::Fpga;
    return std::nullopt;
}

bool validLogDrainReason(std::string_view reason) noexcept
{
    return reason == "gpio0" || reason == "pmbus-alert" ||
           reason == "thermal-warn" || reason == "thermal-overt";
}

const char* outcomeToString(Orchestrator::ResetOutcome o) noexcept
{
    switch (o)
    {
        case Orchestrator::ResetOutcome::Ok:           return "Ok";
        case Orchestrator::ResetOutcome::Timeout:      return "Timeout";
        case Orchestrator::ResetOutcome::NotPresent:   return "NotPresent";
        case Orchestrator::ResetOutcome::Busy:         return "Busy";
        case Orchestrator::ResetOutcome::NotSupported: return "NotSupported";
        case Orchestrator::ResetOutcome::PulseFailed:  return "PulseFailed";
    }
    return "Unknown";
}

// ─── Pending-reply trackers ───────────────────────────────────────────────

/** Carries a captured method call message until its deferred reply is sent.
 *  sd_bus_message_ref keeps the message alive after the dispatcher returns;
 *  sd_bus_message_unref is implicit in the dtor. Single-use (the reply is
 *  sent exactly once; subsequent sends are no-ops).
 */
class PendingReply
{
  public:
    explicit PendingReply(sd_bus_message* m) : msg_(sd_bus_message_ref(m)) {}
    ~PendingReply()
    {
        if (msg_) sd_bus_message_unref(msg_);
    }

    PendingReply(const PendingReply&)            = delete;
    PendingReply& operator=(const PendingReply&) = delete;

    /** Send a single-string reply ("s" signature). Idempotent (no-op
     *  on subsequent calls).
     */
    void replyString(std::string_view val) noexcept
    {
        if (replied_.exchange(true, std::memory_order_relaxed)) return;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_message_new_method_return(msg_, &reply);
        if (r < 0)
        {
            lg2::error("DBusManager: sd_bus_message_new_method_return: {ERR}",
                       "ERR", std::strerror(-r));
            return;
        }
        std::string s(val);
        sd_bus_message_append(reply, "s", s.c_str());
        sd_bus_send(nullptr, reply, nullptr);
        sd_bus_message_unref(reply);
    }

    /** Send a "a{ss}" reply built from a flat key/value vector. */
    void replyStringDict(
        const std::vector<std::pair<std::string, std::string>>& kvs) noexcept
    {
        if (replied_.exchange(true, std::memory_order_relaxed)) return;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_message_new_method_return(msg_, &reply);
        if (r < 0) return;
        sd_bus_message_open_container(reply, 'a', "{ss}");
        for (const auto& [k, v] : kvs)
        {
            sd_bus_message_open_container(reply, 'e', "ss");
            sd_bus_message_append(reply, "ss", k.c_str(), v.c_str());
            sd_bus_message_close_container(reply);
        }
        sd_bus_message_close_container(reply);
        sd_bus_send(nullptr, reply, nullptr);
        sd_bus_message_unref(reply);
    }

  private:
    sd_bus_message*  msg_     = nullptr;
    std::atomic<bool> replied_{false};
};

// (ResetAllTracker removed: aggregation now lives inside
//  Orchestrator::requestResetAllSync, which delivers a single completion
//  callback with the full <descriptorId, ResetOutcome> map. DBusManager
//  just translates that map to a D-Bus a{ss} reply.)

// ─── sd-bus vtable ────────────────────────────────────────────────────────

constexpr sd_bus_vtable kManagerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_NAMES(
        "Reset", "ss",
        SD_BUS_PARAM(target) SD_BUS_PARAM(kind),
        "s",
        SD_BUS_PARAM(result),
        &DBusManager::dispatchReset, 0),
    SD_BUS_METHOD_WITH_NAMES(
        "ResetAll", "ss",
        SD_BUS_PARAM(element_class) SD_BUS_PARAM(kind),
        "a{ss}",
        SD_BUS_PARAM(results),
        &DBusManager::dispatchResetAll, 0),
    SD_BUS_METHOD_WITH_NAMES("SetVrTelemetryEnabled", "sb",
                             SD_BUS_PARAM(target) SD_BUS_PARAM(enabled), "s",
                             SD_BUS_PARAM(result),
                             &DBusManager::dispatchSetVrTelemetryEnabled, 0),
    SD_BUS_METHOD_WITH_NAMES(
        "RequestLogDrain", "ss",
        SD_BUS_PARAM(target) SD_BUS_PARAM(reason),
        "s",
        SD_BUS_PARAM(result),
        &DBusManager::dispatchRequestLogDrain, 0),
    SD_BUS_VTABLE_END,
};

}  // namespace

// ─── Lifecycle ────────────────────────────────────────────────────────────

DBusManager::DBusManager(sdbusplus::bus::bus& bus, Orchestrator& orch,
                        hal::common::SharedLineRegistry& lineRegistry)
    : bus_(bus), orch_(orch), lineRegistry_(lineRegistry)
{
    int r = sd_bus_add_object_vtable(
        get_busp(bus_), &slot_,
        kObjectPath, kInterface,
        kManagerVtable, this);
    if (r < 0)
    {
        // Fatal: without the object vtable, the manager interface is not
        // reachable on D-Bus, so bmcweb (and any other client) cannot drive
        // reset paths. Throwing aborts daemon startup with a clear log
        // message rather than letting the daemon stay up in an apparently-
        // healthy state with a silent missing surface. main() does not
        // catch this, so std::terminate runs and systemd marks the unit
        // as failed -> Restart=on-failure kicks in.
        slot_ = nullptr;
        const std::string err = std::strerror(-r);
        lg2::error("DBusManager: sd_bus_add_object_vtable failed ({ERR}); "
                   "aborting daemon startup",
                   "ERR", err);
        throw std::runtime_error(
            "DBusManager: sd_bus_add_object_vtable failed: " + err);
    }
    lg2::info("DBusManager: registered {IF} on {PATH}",
              "IF", std::string{kInterface}, "PATH", std::string{kObjectPath});
}

DBusManager::~DBusManager()
{
    // Tear down in reverse order of allocation:
    //   1. drainSource_ (defer source attached to the bus's sd_event)
    //   2. slot_ (sd_bus vtable registration)
    // Both are sd-event/sd-bus owned -- unref'ing while still attached to
    // a running loop is safe; the references are auto-detached when the
    // loop iterates next. We drop them eagerly here so that if the daemon
    // is shutting down for restart (sigterm path) the sd-event loop sees
    // a clean refcount when sd_event_unref runs after main() exits.
    if (drainSource_)
    {
        sd_event_source_set_enabled(drainSource_, SD_EVENT_OFF);
        sd_event_source_unref(drainSource_);
        drainSource_ = nullptr;
    }
    if (slot_) sd_bus_slot_unref(slot_);
}

void DBusManager::enqueueNotify_(
    std::shared_ptr<hal::common::SharedLine> line, bool asserted)
{
    pendingQueue_.push_back(PendingNotify{std::move(line), asserted});
    scheduleDrain_();
}

bool DBusManager::injectLineState(const std::string& lineName, bool asserted,
                                  const char* source)
{
    auto sl = lineRegistry_.findByName(lineName);
    if (!sl)
    {
        lg2::warning(
            "DBusManager.{SRC}: no SharedLine registered for '{LINE}' "
            "(asserted={ASS}); ignoring",
            "SRC", source == nullptr ? "injectLineState" : source,
            "LINE", lineName, "ASS", asserted ? 1 : 0);
        return false;
    }

    lg2::info("DBusManager.{SRC}: {LINE} asserted={ASS}",
              "SRC", source == nullptr ? "injectLineState" : source,
              "LINE", lineName, "ASS", asserted ? 1 : 0);
    enqueueNotify_(std::move(sl), asserted);
    return true;
}

void DBusManager::scheduleDrain_()
{
    // Idempotent: if the drain source already exists, we just (re-)arm it
    // in oneshot mode and exit. Whether the previous arming already fired
    // or hasn't fired yet, ONESHOT means it will fire on the NEXT loop
    // iteration, picking up whatever's in pendingQueue_ at that point.
    if (drainSource_)
    {
        sd_event_source_set_enabled(drainSource_, SD_EVENT_ONESHOT);
        return;
    }

    // First-time arming: pull the sd_event loop the bus is attached to,
    // create a defer source bound to drainPendingNotifies_, and arm it
    // in ONESHOT mode. We cache the source for subsequent re-arms above.
    // NB: sd_bus_get_event returns the sd_event* by value (or nullptr if
    // the bus has no event loop attached); it does NOT use an out-param.
    sd_event* loop = sd_bus_get_event(get_busp(bus_));
    if (!loop)
    {
        // No event loop -> degrade to synchronous drain. Should never
        // happen in production (the daemon always attaches its bus to
        // the orchestrator's sd_event); log and process inline so we
        // don't silently drop edges.
        lg2::error("DBusManager.scheduleDrain_: sd_bus_get_event returned "
                   "null (bus not attached to an event loop); falling "
                   "back to synchronous fan-out (this negates the burst-"
                   "latency fix; check daemon wiring)");
        auto pending = std::move(pendingQueue_);
        for (auto& it : pending)
        {
            try { it.line->notifyExternal(it.asserted); }
            catch (const std::exception& e)
            {
                lg2::error("DBusManager: sync fallback notifyExternal({L},"
                           "{A}) threw: {ERR}",
                           "L", it.line->name(),
                           "A", it.asserted ? 1 : 0, "ERR", e.what());
            }
        }
        return;
    }

    int r = sd_event_add_defer(loop, &drainSource_,
                               &DBusManager::drainPendingNotifies_, this);
    if (r < 0)
    {
        drainSource_ = nullptr;
        lg2::error("DBusManager.scheduleDrain_: sd_event_add_defer failed: "
                   "{ERR}; falling back to synchronous fan-out",
                   "ERR", std::strerror(-r));
        auto pending = std::move(pendingQueue_);
        for (auto& it : pending)
        {
            try { it.line->notifyExternal(it.asserted); }
            catch (const std::exception&) { /* logged in dispatcher */ }
        }
        return;
    }
    sd_event_source_set_enabled(drainSource_, SD_EVENT_ONESHOT);
}

int DBusManager::drainPendingNotifies_(sd_event_source* /*s*/, void* userdata)
{
    auto* self = static_cast<DBusManager*>(userdata);
    // Per-drain trace (debug). Records the batch size each drain
    // processed -- under sustained burst this batches >1 items per
    // drain; in steady state it's 1.
    lg2::debug("DBusManager.drain: fired (items={N})",
               "N", static_cast<uint64_t>(self->pendingQueue_.size()));

    // Move the queue out before processing. Any new notifications that
    // arrive during a subscriber callback (re-entrance through D-Bus is
    // possible in theory but doesn't happen on the current code path)
    // get appended to a FRESH pendingQueue_; scheduleDrain_ will re-arm
    // the source for the next iteration via the ONESHOT mechanic.
    auto pending = std::move(self->pendingQueue_);
    self->pendingQueue_.clear();  // explicit; move-from leaves valid-but-unspecified

    for (auto& item : pending)
    {
        try
        {
            item.line->notifyExternal(item.asserted);
        }
        catch (const std::exception& e)
        {
            lg2::error("DBusManager.drain: notifyExternal({LINE}, {ASS}) "
                       "threw: {ERR}",
                       "LINE", item.line->name(),
                       "ASS",  item.asserted ? 1 : 0,
                       "ERR",  e.what());
            // Continue draining -- one bad line should not stall the rest.
        }
    }
    return 0;
}

// ─── Method dispatchers ───────────────────────────────────────────────────

int DBusManager::dispatchReset(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<DBusManager*>(userdata);
    const char* targetC = nullptr;
    const char* kindC   = nullptr;
    int r = sd_bus_message_read(m, "ss", &targetC, &kindC);
    if (r < 0)
    {
        lg2::error("DBusManager.Reset: sd_bus_message_read failed: {ERR}",
                   "ERR", std::strerror(-r));
        return r;
    }
    const std::string target = targetC ? targetC : "";
    const std::string kindStr = kindC ? kindC : "";

    auto reply = std::make_shared<PendingReply>(m);

    auto kind = parseKind(kindStr);
    if (!kind)
    {
        lg2::warning("DBusManager.Reset: invalid kind={KIND} for {TGT}",
                     "KIND", kindStr, "TGT", target);
        reply->replyString("InvalidKind");
        return 1;
    }
    auto inst = self->orch_.instanceByDescriptorId(target);
    if (!inst)
    {
        lg2::warning("DBusManager.Reset: no element for target={TGT}",
                     "TGT", target);
        reply->replyString(outcomeToString(
            Orchestrator::ResetOutcome::NotPresent));
        return 1;
    }

    lg2::info("DBusManager.Reset: target={TGT} kind={KIND} -> async pulse",
              "TGT", target, "KIND", kindStr);
    self->orch_.requestResetAsync(
        *inst, *kind,
        [reply](Orchestrator::ResetOutcome out) {
            reply->replyString(outcomeToString(out));
        });
    return 1;  // 1 = "method handled; reply will follow asynchronously"
}

int DBusManager::dispatchResetAll(
    sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<DBusManager*>(userdata);
    const char* classC = nullptr;
    const char* kindC  = nullptr;
    int r = sd_bus_message_read(m, "ss", &classC, &kindC);
    if (r < 0) return r;
    const std::string elementClass = classC ? classC : "";
    const std::string kindStr      = kindC  ? kindC  : "";

    auto reply = std::make_shared<PendingReply>(m);

    auto cls = parseElementClass(elementClass);
    if (!cls)
    {
        reply->replyStringDict({{"_error", "InvalidElementClass:" + elementClass}});
        return 1;
    }
    auto kind = parseKind(kindStr);
    if (!kind)
    {
        reply->replyStringDict({{"_error", "InvalidKind:" + kindStr}});
        return 1;
    }

    // Dispatch into the orchestrator's batched mass-reset path. That
    // path issues all reset-asserts back-to-back across the present
    // elements, sleeps once for the warm-reset hold, issues all
    // reset-releases back-to-back, and then waits per-element for BC in
    // parallel. The single callback below receives the full
    // <descriptorId, ResetOutcome> map once every wait has settled (or
    // every deadline has fired).
    lg2::info("DBusManager.ResetAll: class={CLS} kind={KIND} batching as "
              "near-simultaneous pulse",
              "CLS", elementClass, "KIND", kindStr);

    self->orch_.requestResetAllSync(
        *cls, *kind,
        [reply](std::vector<std::pair<std::string,
                                      Orchestrator::ResetOutcome>> outcomes) {
            std::vector<std::pair<std::string, std::string>> stringified;
            stringified.reserve(outcomes.size());
            for (auto& [id, out] : outcomes)
            {
                stringified.emplace_back(std::move(id), outcomeToString(out));
            }
            reply->replyStringDict(stringified);
        });
    return 1;
}

int DBusManager::dispatchSetVrTelemetryEnabled(sd_bus_message* m,
                                               void* userdata, sd_bus_error*)
{
    auto* self = static_cast<DBusManager*>(userdata);
    const char* targetValue = nullptr;
    int enabled = 0;
    const int readResult = sd_bus_message_read(m, "sb", &targetValue, &enabled);
    if (readResult < 0)
    {
        lg2::error(
            "DBusManager.SetVrTelemetryEnabled: sd_bus_message_read failed: {ERR}",
            "ERR", std::strerror(-readResult));
        return readResult;
    }

    const std::string target = targetValue == nullptr ? "" : targetValue;
    hal::Result result = hal::Result::NotPresent;
    const auto instance = self->orch_.instanceByDescriptorId(target);
    if (instance && instance->cls == InstanceId::Class::Lpu)
    {
        result = self->orch_.setVrTelemetryConfigured(*instance, enabled != 0);
    }

    lg2::info(
        "DBusManager.SetVrTelemetryEnabled: target={TARGET} enabled={ENABLED} result={RESULT}",
        "TARGET", target, "ENABLED", enabled != 0, "RESULT",
        std::string(hal::to_string(result)));
    const std::string resultText(hal::to_string(result));
    return sd_bus_reply_method_return(m, "s", resultText.c_str());
}

int DBusManager::dispatchRequestLogDrain(
    sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<DBusManager*>(userdata);
    const char* targetC = nullptr;
    const char* reasonC = nullptr;
    const int r = sd_bus_message_read(m, "ss", &targetC, &reasonC);
    if (r < 0)
    {
        return r;
    }
    const std::string target = targetC ? targetC : "";
    const std::string reason = reasonC ? reasonC : "";
    auto reply = std::make_shared<PendingReply>(m);

    if (!validLogDrainReason(reason))
    {
        lg2::warning("DBusManager.RequestLogDrain: invalid reason={REASON} "
                     "target={TARGET}",
                     "REASON", reason, "TARGET", target);
        reply->replyString("InvalidReason");
        return 1;
    }
    const auto instance = self->orch_.instanceByDescriptorId(target);
    if (!instance)
    {
        reply->replyString("NotPresent");
        return 1;
    }

    const auto result = self->orch_.requestLogDrain(*instance, reason);
    lg2::info("DBusManager.RequestLogDrain: target={TARGET} reason={REASON} "
              "result={RESULT}",
              "TARGET", target, "REASON", reason, "RESULT",
              std::string(hal::to_string(result)));
    reply->replyString(hal::to_string(result));
    return 1;
}

}  // namespace pwrseq
