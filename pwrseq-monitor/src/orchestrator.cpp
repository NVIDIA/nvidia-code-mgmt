/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Orchestrator implementation — descriptor-driven instantiation (design §6).
 *
 * Topology comes from a config::PlatformDescriptor (typically loaded at
 * startup from /usr/share/lpu-pwrseq-monitor/platform.json). No counts, no
 * grouping rules, and no chip paths are baked into this file. Adding a tray
 * variant is a JSON edit and a deploy.
 */

#include "orchestrator.hpp"

#include "fsm/state_dump.hpp"
#include "gpio_monitor_read.hpp"
#include "hal/pg_bc_latch.hpp"

#include <phosphor-logging/lg2.hpp>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

namespace pwrseq
{

namespace
{

// Upper bound on how long a Reset/ResetAll D-Bus call blocks waiting for a
// post-reset BcHi edge before returning Timeout (bmcweb maps that to HTTP
// 503). Independent of the FSM boot deadline (tBoot_) so this can be tuned
// without changing fault-detection timing.
//
// TEMPORARY (TS1 bring-up): 20s instead of the prior T_BOOT + 3s (15s on
// LP30 C00). Revert to tBoot_ + 3s after TS1 validation completes.
constexpr auto kResetCompletionTimeout = std::chrono::milliseconds{20'000};
constexpr auto kTelemetryRetryInterval = std::chrono::milliseconds{1'000};
constexpr unsigned kMaxTelemetryRetryAttempts = 30;

constexpr uint64_t packInstance(InstanceId id) noexcept
{
    return (static_cast<uint64_t>(id.cls) << 16) | id.index;
}

constexpr InstanceId unpackInstance(uint64_t key) noexcept
{
    return InstanceId{
        static_cast<InstanceId::Class>(key >> 16),
        static_cast<uint16_t>(key & 0xFFFF),
    };
}

/** Translate a config::LineRef into HAL driver params
 *  (`prefix` → `prefix_line`, `prefix_active_low`). */
void pushLineRefParams(std::vector<std::pair<std::string, std::string>>& out,
                       const std::string& prefix,
                       const config::LineRef& ref)
{
    if (ref.empty()) return;
    out.emplace_back(prefix + "_line",       ref.line);
    out.emplace_back(prefix + "_active_low", ref.activeLow ? "1" : "0");
}

}  // namespace

Orchestrator::Orchestrator(sdbusplus::bus::bus& bus, sd_event* loop) :
    bus_(bus), loop_(loop)
{}

Orchestrator::~Orchestrator()
{
    for (auto* src : ioSources_) sd_event_source_unref(src);
    for (auto& [_, t] : bootTimers_)
    {
        if (t.source) sd_event_source_unref(t.source);
    }
    for (auto& [_, t] : telemetryRetryTimers_)
    {
        if (t.source)
            sd_event_source_unref(t.source);
    }
    // Any pending async resets at shutdown have no path to completion --
    // disable + free their deadline timers so sd_event doesn't fire them
    // on a destroyed Orchestrator. Their callbacks are dropped silently;
    // bmcweb-side D-Bus replies time out via the normal d-bus method
    // timeout, which is the right behaviour on daemon shutdown.
    for (auto& [_, p] : pendingResets_)
    {
        if (!p) continue;
        if (p->deadline)
        {
            sd_event_source_set_enabled(p->deadline, SD_EVENT_OFF);
            sd_event_source_unref(p->deadline);
        }
    }
}

// ── Descriptor → DeviceConfig conversion ──────────────────────────────────

hal::DeviceConfig Orchestrator::buildLpuConfig_(
    InstanceId id, const config::ElementDescriptor& el)
{
    hal::DeviceConfig cfg;
    cfg.instanceId    = id;
    cfg.initTimeoutMs = tBoot_;
    cfg.sharedLines   = &sharedLines_;
    cfg.resetChips    = &resetChips_;
    pushLineRefParams(cfg.driverParams, "pg", el.pg);
    pushLineRefParams(cfg.driverParams, "bc", el.bc);
    pushLineRefParams(cfg.driverParams, "reset", el.resetLine);
    if (!el.telemetry.logDev.empty())
    {
        cfg.driverParams.emplace_back("log_dev", el.telemetry.logDev);
    }
    return cfg;
}

hal::DeviceConfig Orchestrator::buildFpgaConfig_(
    InstanceId id, const config::FpgaDescriptor& f)
{
    hal::DeviceConfig cfg;
    cfg.instanceId    = id;
    cfg.initTimeoutMs = tBoot_;
    cfg.sharedLines   = &sharedLines_;
    cfg.resetChips    = &resetChips_;
    pushLineRefParams(cfg.driverParams, "pg", f.pg);
    pushLineRefParams(cfg.driverParams, "bc", f.bc);
    pushLineRefParams(cfg.driverParams, "reset", f.resetLine);
    return cfg;
}

// ── Redfish path helpers (descriptor IDs are authoritative) ───────────────

std::string Orchestrator::elementRedfishPath_(
    const config::ElementDescriptor& e)
{
    if (e.kind == config::ElementDescriptor::Kind::Lpu)
    {
        // bmcweb's Processor handler (handleProcessorGet/getProcessorData in
        // redfish-core/lib/processor.hpp + nvidia_processor.hpp) looks up
        // the inventory object under
        //   /xyz/openbmc_project/inventory/system/accelerator/<RedfishMemberId>
        // not under /board/.../Processor/HGX_<id>. Strip the descriptor's
        // "HGX_" namespace prefix so the leaf matches the Redfish member id
        // ("LPU_0", not "HGX_LPU_0"), and publish at the accelerator path
        // so bmcweb actually finds our Health + OperationalStatus interfaces
        // via the ObjectMapper.
        std::string redfishId = e.id;
        constexpr std::string_view kHgxPrefix = "HGX_";
        if (redfishId.starts_with(kHgxPrefix))
        {
            redfishId.erase(0, kHgxPrefix.size());
        }
        return "/xyz/openbmc_project/inventory/system/accelerator/" + redfishId;
    }
    return "/xyz/openbmc_project/inventory/system/chassis/" + e.id;
}

std::string Orchestrator::moduleRedfishPath_(const config::ModuleDescriptor& m)
{
    return "/xyz/openbmc_project/inventory/system/chassis/" + m.id;
}

std::string Orchestrator::fpgaRedfishPath_(const config::FpgaDescriptor& f)
{
    return "/xyz/openbmc_project/inventory/system/chassis/" + f.id;
}

std::string Orchestrator::trayRedfishPath_(const std::string& chassisId)
{
    return "/xyz/openbmc_project/inventory/system/chassis/" + chassisId;
}

// ── Main entry point ──────────────────────────────────────────────────────

hal::Result Orchestrator::initFromDescriptor(
    const config::PlatformDescriptor& desc)
{
    tBoot_ = desc.tBootMs;
    platform_ = desc.platform;

    lg2::info("pwrseq-monitor: initialising from descriptor "
              "(platform={PLAT}, modules={NM}, fpgas={NF}, t_boot_ms={TB})",
              "PLAT", desc.platform,
              "NM", static_cast<int>(desc.modules.size()),
              "NF", static_cast<int>(desc.fpgas.size()),
              "TB", static_cast<int>(desc.tBootMs.count()));

    // Tally elements/FPGAs that were declared in the descriptor but could
    // not be instantiated (driver init failed, unsupported kind, etc.).
    // If any non-zero, the function returns hal::Result::InitFailed at the
    // end so main() can log/alert that the daemon is running in a degraded
    // (partially-wired) state instead of treating it as fully healthy.
    std::size_t declaredCount = 0;
    std::size_t failedCount   = 0;

    // ── 1. Tray FSM + reporter ──────────────────────────────────────────
    constexpr InstanceId kTrayId{InstanceId::Class::Tray, 0};
    trayFsm_ = std::make_unique<fsm::TrayFsm>(
        kTrayId,
        /*numModules=*/static_cast<uint16_t>(desc.modules.size()),
        /*numFpgas=*/  static_cast<uint16_t>(desc.fpgas.size()));
    reporters_.push_back(std::make_unique<reporting::Reporter>(
        bus_, trayRedfishPath_(desc.chassisId)));
    instanceByDescId_[desc.chassisId] = packInstance(kTrayId);
    descIdByInstance_[packInstance(kTrayId)] = desc.chassisId;
    wireTrayFsm_(*trayFsm_, *reporters_.back());

    // ── 2. Module FSMs + element FSMs/drivers ───────────────────────────
    // Allocate sequential 0-based indices in descriptor walk order.
    uint16_t nextElementIdx = 0;
    for (uint16_t mi = 0; mi < desc.modules.size(); ++mi)
    {
        const auto& m = desc.modules[mi];
        InstanceId modId{InstanceId::Class::Module, mi};

        const auto childCount = static_cast<uint16_t>(m.elements.size());
        if (childCount > 2)
        {
            // ModuleFsm currently hard-codes a 2-element child array
            // (the LP30 silicon ships exactly 2 LPUs per module). Silently
            // wiring extra children would still allocate ElementFsms for
            // them but they would never be aggregated into the parent
            // module's state, so the module/tray layers would report
            // stale or wrong rollups. Skip the whole module loudly until
            // ModuleFsm generalises to N children (design ref: §2.2).
            lg2::error("Module {MID} has {N} elements but ModuleFsm only "
                       "supports up to 2; SKIPPING this module entirely "
                       "to avoid silent mis-aggregation at the tray layer",
                       "MID", m.id, "N", static_cast<int>(childCount));
            failedCount += childCount;
            declaredCount += childCount;
            continue;
        }
        if (childCount < 1)
        {
            lg2::warning("Module {MID} has zero elements; skipping",
                         "MID", m.id);
            continue;
        }

        InstanceId childA{InstanceId::Class::Lpu, nextElementIdx};
        InstanceId childB{InstanceId::Class::Lpu,
                          static_cast<uint16_t>(nextElementIdx + 1)};
        if (childCount < 2)
        {
            // Reuse childA as both slots — ModuleFsm will treat the missing
            // slot as Stby and aggregate accordingly.
            childB = childA;
        }

        auto modFsm = std::make_unique<fsm::ModuleFsm>(modId, childA, childB);
        reporters_.push_back(std::make_unique<reporting::Reporter>(
            bus_, moduleRedfishPath_(m)));
        wireModuleFsm_(*modFsm, *reporters_.back());
        moduleByInstance_[packInstance(modId)] = modFsm.get();
        instanceByDescId_[m.id] = packInstance(modId);
        descIdByInstance_[packInstance(modId)] = m.id;
        moduleFsms_.push_back(std::move(modFsm));

        for (uint16_t ei = 0; ei < childCount; ++ei)
        {
            const auto& el = m.elements[ei];
            InstanceId elemId{InstanceId::Class::Lpu, nextElementIdx++};
            ++declaredCount;

            auto fsmObj = std::make_unique<fsm::ElementFsm>(elemId);
            std::unique_ptr<hal::IDevice> driver;
            switch (el.kind)
            {
                case config::ElementDescriptor::Kind::Lpu:
                    driver = std::make_unique<hal::LpuDriver>(elemId);
                    break;
                case config::ElementDescriptor::Kind::Mcu:
                case config::ElementDescriptor::Kind::Gpu:
                    lg2::error("Element kind {KIND} not yet supported "
                               "(id={EID}); slot left absent",
                               "KIND", static_cast<int>(el.kind),
                               "EID", el.id);
                    ++failedCount;
                    continue;
            }

            const auto ir = driver->init(*this, buildLpuConfig_(elemId, el));
            if (ir == hal::Result::Absent)
            {
                lg2::info("LPU element {EID} absent (module not populated), skipping",
                          "EID", el.id);
                continue;
            }
            if (ir != hal::Result::Ok)
            {
                lg2::error("Driver init failed for element {EID}", "EID", el.id);
                ++failedCount;
                continue;
            }

            reporters_.push_back(std::make_unique<reporting::Reporter>(
                bus_, elementRedfishPath_(el)));
            wireElementFsm_(*fsmObj, *driver, *reporters_.back(),
                            /*isLpu=*/true);
            registerDeviceFds_(*driver);

            elementByInstance_[packInstance(elemId)] = fsmObj.get();
            deviceByInstance_[packInstance(elemId)]  = driver.get();
            elementToModule_[packInstance(elemId)]   = packInstance(modId);
            instanceByDescId_[el.id]                 = packInstance(elemId);
            descIdByInstance_[packInstance(elemId)]  = el.id;

            // FSM seeding: Gpio.Monitor.Line PropertiesChanged / ObjectManager
            // snapshot injects each PG/BC Bound Value once the subscriber
            // is started (see main.cpp).

            devices_.push_back(std::move(driver));
            elementFsms_.push_back(std::move(fsmObj));
        }
    }

    // ── 3. FPGA Element FSMs + drivers ──────────────────────────────────
    for (uint16_t fi = 0; fi < desc.fpgas.size(); ++fi)
    {
        const auto& f = desc.fpgas[fi];
        InstanceId fpgaId{InstanceId::Class::Fpga, fi};
        ++declaredCount;

        auto fpgaFsm = std::make_unique<fsm::ElementFsm>(fpgaId);
        auto driver  = std::make_unique<hal::FpgaDriver>(fpgaId);

        const auto fr = driver->init(*this, buildFpgaConfig_(fpgaId, f));
        if (fr == hal::Result::Absent)
        {
            lg2::info("FPGA {FID} absent (not populated), skipping",
                      "FID", f.id);
            continue;
        }
        if (fr != hal::Result::Ok)
        {
            lg2::error("FpgaDriver init failed for {FID}", "FID", f.id);
            ++failedCount;
            continue;
        }

        reporters_.push_back(std::make_unique<reporting::Reporter>(
            bus_, fpgaRedfishPath_(f)));
        wireElementFsm_(*fpgaFsm, *driver, *reporters_.back(),
                        /*isLpu=*/false);
        registerDeviceFds_(*driver);

        elementByInstance_[packInstance(fpgaId)] = fpgaFsm.get();
        deviceByInstance_[packInstance(fpgaId)]  = driver.get();
        instanceByDescId_[f.id]                  = packInstance(fpgaId);
        descIdByInstance_[packInstance(fpgaId)]  = f.id;

        devices_.push_back(std::move(driver));
        elementFsms_.push_back(std::move(fpgaFsm));
    }

    // Input lines are edge-driven exclusively via Gpio.Monitor.Line
    // PropertiesChanged (property subscriber → injectLineState). The daemon
    // never opens PG/BC chardevs; registerDeviceFds_ is a no-op for inputs.

    if (failedCount > 0)
    {
        lg2::error("Orchestrator: degraded init -- {OK}/{TOTAL} declared "
                   "element(s)/FPGA(s) wired; {BAD} could not be "
                   "instantiated (driver init failure or unsupported kind). "
                   "Daemon will run but partial reset/telemetry surfaces are "
                   "missing for the failed entries.",
                   "OK",    static_cast<int>(declaredCount - failedCount),
                   "TOTAL", static_cast<int>(declaredCount),
                   "BAD",   static_cast<int>(failedCount));
        return hal::Result::InitFailed;
    }
    return hal::Result::Ok;
}

// ── FSM wiring ────────────────────────────────────────────────────────────

void Orchestrator::wireElementFsm_(fsm::ElementFsm& fsm, hal::IDevice& dev,
                                   reporting::Reporter& rep, bool isLpu)
{
    const InstanceId id = fsm.ctx().id;

    fsm.setStartTBoot ([this, id]() { startBootTimer_(id); });
    fsm.setCancelTBoot([this, id]() { cancelBootTimer_(id); });

    fsm.setOnStateChange(
        [this, id, &fsm, &dev, &rep, isLpu](ElementState old, ElementState neu) {
            // Always-on transition trace. Field debug + off-target test rig
            // both consume these lines via `journalctl -u lpu-pwrseq-monitord`.
            // Format is stable: `FSM: element <id> <from> -> <to> [fault=<fc>]`.
            if (neu == ElementState::Fault)
            {
                lg2::info("FSM: element {ID} {FROM} -> {TO} fault={FAULT}",
                          "ID",    to_string(id),
                          "FROM",  std::string(to_string(old)),
                          "TO",    std::string(to_string(neu)),
                          "FAULT", std::string(to_string(fsm.faultCode())));
            }
            else
            {
                lg2::info("FSM: element {ID} {FROM} -> {TO}",
                          "ID",   to_string(id),
                          "FROM", std::string(to_string(old)),
                          "TO",   std::string(to_string(neu)));
            }

            rep.applyStatus(neu, /*present=*/true);

            if (isLpu && (old == ElementState::Online
                          || neu == ElementState::Online))
            {
                cancelTelemetryRetry_(id);
                const bool enable = neu == ElementState::Online;
                const auto pr = dev.setTelemetryPolling(enable);
                if (enable && pr == hal::Result::NotReady)
                {
                    scheduleTelemetryRetry_(id, 1);
                }
                else if (pr != hal::Result::Ok
                         && pr != hal::Result::NotSupported)
                {
                    lg2::warning("FSM: element {ID} telemetry polling "
                                 "ioctl failed ({RC}) on {FROM} -> {TO}",
                                 "ID",   to_string(id),
                                 "RC",   std::string(hal::to_string(pr)),
                                 "FROM", std::string(to_string(old)),
                                 "TO",   std::string(to_string(neu)));
                }
            }

            if (isLpu)
            {
                auto pit = elementToModule_.find(packInstance(id));
                if (pit == elementToModule_.end()) return;
                auto mit = moduleByInstance_.find(pit->second);
                if (mit == moduleByInstance_.end()) return;

                InstanceId modId{
                    static_cast<InstanceId::Class>(pit->second >> 16),
                    static_cast<uint16_t>(pit->second & 0xFFFF)};

                mit->second->dispatch(SemanticEvent{
                    EventKind::LpuStateChanged, modId,
                    ChildElementPayload{id, neu},
                });
            }
            else if (trayFsm_)
            {
                trayFsm_->dispatch(SemanticEvent{
                    EventKind::FpgaStateChanged, trayFsm_->ctx().id,
                    ChildElementPayload{id, neu},
                });
            }
        });

    // GPIO seeding can report deasserted PG/BC while the FSM is already in
    // Stby.  Those are valid self-transitions, so onStateChange is not called.
    // Publish the baseline explicitly so every configured resource exists on
    // D-Bus even when its first samples do not change the FSM state.
    rep.applyStatus(fsm.state(), /*present=*/true);
}

void Orchestrator::wireModuleFsm_(fsm::ModuleFsm& fsm,
                                  reporting::Reporter& rep)
{
    const InstanceId id = fsm.ctx().id;
    fsm.setOnStateChange(
        [this, id, &rep](ModuleState old, ModuleState neu) {
            lg2::info("FSM: module {ID} {FROM} -> {TO}",
                      "ID",   to_string(id),
                      "FROM", std::string(to_string(old)),
                      "TO",   std::string(to_string(neu)));

            rep.applyStatus(neu);
            if (trayFsm_)
            {
                trayFsm_->dispatch(SemanticEvent{
                    EventKind::ModuleStateChanged, trayFsm_->ctx().id,
                    ChildModulePayload{id, neu},
                });
            }
        });

    rep.applyStatus(fsm.state());
}

void Orchestrator::wireTrayFsm_(fsm::TrayFsm& fsm, reporting::Reporter& rep)
{
    const InstanceId id = fsm.ctx().id;
    fsm.setOnStateChange(
        [id, &rep](TrayState old, TrayState neu) {
            lg2::info("FSM: tray {ID} {FROM} -> {TO}",
                      "ID",   to_string(id),
                      "FROM", std::string(to_string(old)),
                      "TO",   std::string(to_string(neu)));

            rep.applyStatus(neu);
        });

    rep.applyStatus(fsm.state());
}

// ── sd-event glue ────────────────────────────────────────────────────────

void Orchestrator::registerDeviceFds_(hal::IDevice& dev)
{
    for (int fd : dev.fds())
    {
        // D-Bus-sourced SharedLines (those receiving edges via
        // Gpio.Monitor.Line PropertiesChanged instead of a kernel chardev
        // event fd) report fd == -1. They have no fd to register; events
        // are routed into the SharedLine's subscriber list by the GPIO
        // property subscriber. Skip rather than feed sd_event_add_io a
        // negative fd (which would EBADF the entire loop).
        if (fd < 0)
        {
            continue;
        }
        // Multiple drivers may report the same fd (they share a SharedLine
        // for a board-level signal — e.g. both LPUs on a C00 module return
        // MODULE_PGOOD_R's fd). Register each unique fd exactly once with
        // sd-event; the first driver that owns the fd receives onReadable
        // and its SharedLine fans the events out to every subscriber, which
        // includes the other drivers that share the line.
        if (!registeredFds_.insert(fd).second)
        {
            continue;
        }

        sd_event_source* src = nullptr;
        auto* devPtr = &dev;
        struct CtxCallback {
            hal::IDevice* device;
            int           fd;
            Orchestrator* self;
        };
        auto* ctx = new CtxCallback{devPtr, fd, this};

        sd_event_add_io(loop_, &src, fd, EPOLLIN,
                        [](sd_event_source*, int /*fd*/, uint32_t,
                           void* userdata) -> int {
                            auto* c = static_cast<CtxCallback*>(userdata);
                            c->self->onDeviceReadable_(c->device, c->fd);
                            return 0;
                        },
                        ctx);
        ioSources_.push_back(src);
    }
}

void Orchestrator::onDeviceReadable_(hal::IDevice* dev, int fd)
{
    dev->onReadable(fd);
}

void Orchestrator::emit(const SemanticEvent& ev)
{
    const uint64_t key = packInstance(ev.target);

    // Normal path: route the event to the target Element FSM.
    auto it = elementByInstance_.find(key);
    if (it != elementByInstance_.end())
    {
        it->second->dispatch(ev);
    }

    // Async-reset completion path: if there is a pending reset for this
    // target AND the event is BcHi, that's "the reset succeeded" -- the
    // element rebooted and asserted boot-complete. Complete the callback
    // (which sends the deferred D-Bus reply); the timer is unref'd by
    // completePending_. Done AFTER the FSM dispatch above so the FSM has
    // already walked Powered -> Online before the caller observes the
    // result (matters when the caller immediately queries state).
    if (ev.kind == EventKind::BcHi)
    {
        auto pit = pendingResets_.find(key);
        if (pit != pendingResets_.end())
        {
            completePending_(key, ResetOutcome::Ok);
        }
    }
}

hal::Result Orchestrator::requestReset(InstanceId target, hal::ResetKind kind)
{
    auto devIt = deviceByInstance_.find(packInstance(target));
    if (devIt == deviceByInstance_.end()) return hal::Result::NotPresent;
    auto* dev = devIt->second;

    // One-shot reset: dispatch RstAsserted, trigger the device's reset
    // line (LSTP assert-only on LP30), dispatch RstReleased. The Element
    // FSM handles the rest — it re-arms T_BOOT on entry to S_POWERED via
    // the RstReleased row. Completion is observed via BC, not reset-pin
    // release.
    //
    // Invalidate the BC SharedLine latch before starting the new boot
    // cycle so a pre-reset BOOT_COMPLETE level cannot be reused on a
    // later PgHi sample (NVBUG 6559782). Async callers also clear before
    // arming the completion wait; this covers the sync path.
    dev->clearBootCompleteCache();

    auto fsmIt = elementByInstance_.find(packInstance(target));
    if (fsmIt != elementByInstance_.end())
    {
        fsmIt->second->dispatch(SemanticEvent{EventKind::RstAsserted, target});
    }
    auto r = dev->reset(kind);
    if (fsmIt != elementByInstance_.end())
    {
        fsmIt->second->dispatch(SemanticEvent{EventKind::RstReleased, target});
    }
    return r;
}

hal::Result Orchestrator::requestLogDrain(InstanceId target,
                                          std::string_view reason)
{
    if (target.cls != InstanceId::Class::Lpu)
    {
        return hal::Result::BadArgument;
    }
    const auto key = packInstance(target);
    const auto fsmIt = elementByInstance_.find(key);
    if (fsmIt == elementByInstance_.end() || fsmIt->second == nullptr)
    {
        return hal::Result::NotPresent;
    }
    if (fsmIt->second->state() != ElementState::Online)
    {
        lg2::info("requestLogDrain: target={TARGET} reason={REASON} skipped; "
                  "state={STATE}",
                  "TARGET", to_string(target), "REASON", std::string(reason),
                  "STATE", std::string(to_string(fsmIt->second->state())));
        return hal::Result::Ok;
    }

    const auto devIt = deviceByInstance_.find(key);
    if (devIt == deviceByInstance_.end() || devIt->second == nullptr)
    {
        return hal::Result::NotPresent;
    }
    return devIt->second->requestLogDrain(reason);
}

// ── Async reset (Phase 1 part 2) ──────────────────────────────────────────

void Orchestrator::completePending_(uint64_t key, ResetOutcome outcome)
{
    auto it = pendingResets_.find(key);
    if (it == pendingResets_.end()) return;

    // Move out + erase BEFORE invoking the callback so re-entrant callers
    // (e.g. callback that fires another requestResetAsync for the same
    // target) see a clean state.
    auto pending = std::move(it->second);
    pendingResets_.erase(it);

    if (pending->deadline)
    {
        sd_event_source_set_enabled(pending->deadline, SD_EVENT_OFF);
        sd_event_source_unref(pending->deadline);
        pending->deadline = nullptr;
    }
    if (pending->callback)
    {
        pending->callback(outcome);
    }
}

void Orchestrator::requestResetAsync(
    InstanceId target,
    hal::ResetKind kind,
    std::function<void(ResetOutcome)> callback)
{
    const uint64_t key = packInstance(target);

    // Reject if another reset is already in flight for this element.
    // (Mass-reset issues N parallel calls -- one per element -- so this
    // only conflicts on a per-element retry which is operator error.)
    if (pendingResets_.contains(key))
    {
        lg2::warning("requestResetAsync: reset for {ID} already in flight; "
                     "rejecting concurrent request",
                     "ID", to_string(target));
        if (callback) callback(ResetOutcome::Busy);
        return;
    }

    // Validate the target.
    auto devIt = deviceByInstance_.find(key);
    if (devIt == deviceByInstance_.end())
    {
        if (callback) callback(ResetOutcome::NotPresent);
        return;
    }
    if (!resetEligible_(target))
    {
        lg2::info("requestResetAsync: {ID} never powered (no PgHi yet); "
                  "rejecting reset",
                  "ID", to_string(target));
        if (callback) callback(ResetOutcome::NotPresent);
        return;
    }
    auto* dev = devIt->second;

    // Arm the BC wait before actuating reset so a fast post-reset BcHi edge
    // is not missed between pulse return and pending registration.
    if (!armResetCompletionWait_(target, dev, std::move(callback)))
    {
        return;
    }

    dev->clearBootCompleteCache();

    // Trigger reset synchronously (in-process libgpiod assert on reset_line).
    // Daemon waits for post-reset BcHi via gpio-mon below.
    auto pulseR = requestReset(target, kind);  // dispatches Rst{Asserted,Released}
    if (pulseR == hal::Result::NotSupported)
    {
        completePending_(key, ResetOutcome::NotSupported);
        return;
    }
    if (pulseR == hal::Result::Absent)
    {
        completePending_(key, ResetOutcome::NotPresent);
        return;
    }
    if (pulseR != hal::Result::Ok)
    {
        completePending_(key, ResetOutcome::PulseFailed);
        return;
    }
}

bool Orchestrator::armResetCompletionWait_(
    InstanceId target,
    hal::IDevice* /*dev*/,
    std::function<void(ResetOutcome)> callback)
{
    const uint64_t key = packInstance(target);

    // Always wait for a fresh BcHi edge (or the reset-completion timeout).
    // Do not treat a pre-reset BC cache level as completion at arm time —
    // CONF_DONE/CVP can stay high through an N_CONFIG pulse. If the edge
    // never arrives, tryRecoverStickyBc_ on timeout re-reads gpio-mon and
    // may still complete Ok when the pin is still asserted.
    auto pending           = std::make_unique<PendingReset>();
    pending->target        = target;
    pending->callback      = std::move(callback);

    // Reset-completion budget is deliberately independent of the FSM boot
    // deadline (tBoot_): tBoot_ governs fault detection after a re-arm,
    // whereas this deadline bounds how long the Reset/ResetAll D-Bus call
    // blocks before returning Timeout (which bmcweb maps to HTTP 503).
    // TEMPORARY (TS1): see kResetCompletionTimeout; revert after TS1.
    const auto deadlineMs  = kResetCompletionTimeout;
    uint64_t now = 0;
    sd_event_now(loop_, CLOCK_MONOTONIC, &now);
    const uint64_t at = now + std::chrono::duration_cast<
        std::chrono::microseconds>(deadlineMs).count();

    // Capture target only (not `this`) for the C callback -- the userdata
    // pointer carries Orchestrator + key together. The ctx lifetime is
    // tied to the sd_event_source via set_destroy_callback so it is freed
    // regardless of which path completes (deadline fires, emit() via BcHi,
    // or shutdown). Without
    // the destroy callback we leak the ctx whenever completion races with
    // the timer (the BcHi-via-emit path, which is the common case).
    struct TimerCtx
    {
        Orchestrator* self;
        uint64_t      key;
        InstanceId    target;
    };
    auto* ctx = new TimerCtx{this, key, target};

    sd_event_source* src = nullptr;
    int rc = sd_event_add_time(
        loop_, &src, CLOCK_MONOTONIC, at, 0,
        [](sd_event_source* /*s*/, uint64_t /*usec*/, void* userdata) -> int {
            auto* c = static_cast<TimerCtx*>(userdata);
            // Sticky BC: no rising edge arrived; re-check gpio-mon level.
            if (c->self->tryRecoverStickyBc_(c->target))
            {
                // SharedLine notify → BcHi → emit() → completePending_(Ok).
                // If pending remains (no subscriber), complete Ok here.
                if (c->self->pendingResets_.count(c->key))
                {
                    c->self->completePending_(c->key, ResetOutcome::Ok);
                }
                return 0;
            }
            c->self->completePending_(c->key, ResetOutcome::Timeout);
            return 0;  // destroy_callback frees ctx
        },
        ctx);
    if (rc < 0)
    {
        delete ctx;
        lg2::error("armResetCompletionWait_: sd_event_add_time failed for "
                   "{ID}; completing PulseFailed",
                   "ID", to_string(target));
        if (pending->callback) pending->callback(ResetOutcome::PulseFailed);
        return false;
    }
    sd_event_source_set_destroy_callback(src, +[](void* userdata) {
        delete static_cast<TimerCtx*>(userdata);
    });
    pending->deadline = src;
    pendingResets_.emplace(key, std::move(pending));

    lg2::info("armResetCompletionWait_: armed for {ID}; waiting up to "
              "{MS}ms for BcHi via gpio-mon",
              "ID", to_string(target),
              "MS", static_cast<int>(deadlineMs.count()));
    return true;
}

void Orchestrator::requestResetAllSync(
    InstanceId::Class cls,
    hal::ResetKind kind,
    std::function<void(std::vector<std::pair<std::string, ResetOutcome>>)>
        callback)
{
    auto present = presentInstances(cls);
    if (present.empty())
    {
        if (callback) callback({});
        return;
    }

    // Drop never-powered elements (still S_STBY with no PgHi). Empty module
    // bays may still init when the platform descriptor names their GPIO lines
    // but gpio-mon never asserts PG for them.
    std::vector<InstanceId> eligible;
    eligible.reserve(present.size());
    for (auto inst : present)
    {
        if (resetEligible_(inst))
        {
            eligible.push_back(inst);
        }
        else
        {
            lg2::info("requestResetAllSync: {ID} never powered (no PgHi yet); "
                      "skipping in batch",
                      "ID", to_string(inst));
        }
    }
    present = std::move(eligible);
    if (present.empty())
    {
        if (callback) callback({});
        return;
    }

    // Reject any element that's already mid-reset. We pre-check the
    // whole set so we don't half-arm: either the entire batch goes or
    // we surface Busy for the conflicting elements and proceed with the
    // rest. (Operator can re-issue per-element via requestResetAsync.)
    std::vector<InstanceId>            okToReset;
    std::vector<std::pair<std::string, ResetOutcome>> earlyResults;
    okToReset.reserve(present.size());
    earlyResults.reserve(present.size());
    for (auto inst : present)
    {
        std::string descId = descriptorIdByInstance(inst);
        if (descId.empty()) descId = to_string(inst);
        if (pendingResets_.contains(packInstance(inst)))
        {
            lg2::warning("requestResetAllSync: {ID} already mid-reset; "
                         "reporting Busy and skipping in batch",
                         "ID", to_string(inst));
            earlyResults.emplace_back(std::move(descId), ResetOutcome::Busy);
            continue;
        }
        okToReset.push_back(inst);
    }

    // Phase 1: trigger every reset line back-to-back. Dispatch the FSM
    // RstAsserted event for each too, so the element FSMs walk into
    // S_RESETTING in lockstep with the LSTP command. Any assert that
    // fails is recorded as PulseFailed / NotSupported; its target is
    // dropped from the completion-wait phase.
    struct PulseStep
    {
        InstanceId    target;
        std::string   descId;
        hal::IDevice* dev;
        bool          waitArmed = false;
        hal::Result   assertResult;
    };
    std::vector<PulseStep> steps;
    steps.reserve(okToReset.size());

    for (auto inst : okToReset)
    {
        auto* dev = deviceByInstance_[packInstance(inst)];
        std::string descId = descriptorIdByInstance(inst);
        if (descId.empty()) descId = to_string(inst);
        steps.push_back(
            {inst, std::move(descId), dev, /*waitArmed=*/false,
             hal::Result::Ok});
    }

    struct Tracker
    {
        std::vector<std::pair<std::string, ResetOutcome>> results;
        std::size_t expected = 0;
        std::function<void(std::vector<std::pair<std::string, ResetOutcome>>)>
            callback;

        void add(std::string descId, ResetOutcome out)
        {
            results.emplace_back(std::move(descId), out);
            if (results.size() >= expected && callback)
            {
                auto cb = std::move(callback);
                cb(std::move(results));
            }
        }
    };
    auto tracker = std::make_shared<Tracker>();
    tracker->callback = std::move(callback);
    tracker->results  = std::move(earlyResults);
    tracker->expected = tracker->results.size() + steps.size();

    if (tracker->results.size() >= tracker->expected)
    {
        if (tracker->callback)
        {
            auto cb = std::move(tracker->callback);
            cb(std::move(tracker->results));
        }
        return;
    }

    // Arm BC waits and clear stale cache before any reset line is actuated.
    for (auto& s : steps)
    {
        std::string descIdCopy = s.descId;
        if (!armResetCompletionWait_(
                s.target, s.dev,
                [tracker, descIdCopy = std::move(descIdCopy)](ResetOutcome out)
                    mutable {
                    tracker->add(std::move(descIdCopy), out);
                }))
        {
            continue;
        }
        s.waitArmed = true;
        s.dev->clearBootCompleteCache();
    }

    for (auto& s : steps)
    {
        if (!s.waitArmed)
        {
            continue;
        }
        // Dispatch RstAsserted FIRST so the FSM sees the assert ordering
        // even on driver pulse failure (FSM rescue rows handle the
        // S_FAULT case correctly).
        auto fsmIt = elementByInstance_.find(packInstance(s.target));
        if (fsmIt != elementByInstance_.end())
        {
            fsmIt->second->dispatch(
                SemanticEvent{EventKind::RstAsserted, s.target});
        }
        s.assertResult = s.dev->resetAssert(kind);
    }

    // Phase 2: dispatch RstReleased for each successful trigger. On LSTP/
    // USB platforms the module MCU clears the reset net; vHMC does not
    // drive release on the reset output line.
    for (auto& s : steps)
    {
        if (!s.waitArmed)
        {
            continue;
        }
        if (s.assertResult != hal::Result::Ok)
        {
            ResetOutcome out = ResetOutcome::PulseFailed;
            if (s.assertResult == hal::Result::NotSupported)
            {
                out = ResetOutcome::NotSupported;
            }
            else if (s.assertResult == hal::Result::Absent)
            {
                out = ResetOutcome::NotPresent;
            }
            completePending_(packInstance(s.target), out);
            continue;
        }
        auto fsmIt = elementByInstance_.find(packInstance(s.target));
        if (fsmIt != elementByInstance_.end())
        {
            fsmIt->second->dispatch(
                SemanticEvent{EventKind::RstReleased, s.target});
        }
    }

    lg2::info("requestResetAllSync: triggered reset on {N} element(s) of "
              "class={CLS}; now waiting per-element for BcHi",
              "N", static_cast<int>(steps.size()),
              "CLS", static_cast<int>(cls));
}

bool Orchestrator::dumpState(const std::string& path) const
{
    FsmSnapshot snap;
    snap.timestamp = utcTimestamp();
    snap.platform = platform_;

    if (trayFsm_)
    {
        snap.tray.id = descriptorIdByInstance(trayFsm_->ctx().id);
        if (snap.tray.id.empty())
        {
            snap.tray.id = to_string(trayFsm_->ctx().id);
        }
        snap.tray.state = trayFsm_->state();
    }

    snap.modules.reserve(moduleFsms_.size());
    for (const auto& mod : moduleFsms_)
    {
        if (!mod)
        {
            continue;
        }
        ModuleSnapshot row;
        row.id = descriptorIdByInstance(mod->ctx().id);
        if (row.id.empty())
        {
            row.id = to_string(mod->ctx().id);
        }
        row.state = mod->state();
        std::unordered_set<uint64_t> seenChildren;
        for (const InstanceId child : mod->ctx().childIds)
        {
            if (!seenChildren.insert(packInstance(child)).second)
            {
                continue;
            }
            std::string childId = descriptorIdByInstance(child);
            if (childId.empty())
            {
                childId = to_string(child);
            }
            row.children.push_back(std::move(childId));
        }
        snap.modules.push_back(std::move(row));
    }

    snap.elements.reserve(elementFsms_.size());
    for (const auto& el : elementFsms_)
    {
        if (!el)
        {
            continue;
        }
        const InstanceId id = el->ctx().id;
        const uint64_t key = packInstance(id);
        ElementSnapshot row;
        row.id = descriptorIdByInstance(id);
        if (row.id.empty())
        {
            row.id = to_string(id);
        }
        row.className = std::string(to_string(id.cls));
        row.state = el->state();
        row.fault = el->faultCode();
        row.seenPgHi = el->hasSeenPgHi();
        const auto timerIt = bootTimers_.find(key);
        row.bootTimerArmed = timerIt != bootTimers_.end() &&
                             timerIt->second.source != nullptr;
        row.resetPending = pendingResets_.contains(key);
        snap.elements.push_back(std::move(row));
    }

    sharedLines_.for_each_alive([&snap](const hal::common::SharedLine& line) {
        snap.lines.push_back(LineSnapshot{
            .name = line.name(),
            .hasValue = line.hasValue(),
            .asserted = line.read(),
            .activeLow = line.activeLow(),
        });
    });

    std::sort(snap.modules.begin(), snap.modules.end(),
              [](const ModuleSnapshot& a, const ModuleSnapshot& b) {
                  return a.id < b.id;
              });
    std::sort(snap.elements.begin(), snap.elements.end(),
              [](const ElementSnapshot& a, const ElementSnapshot& b) {
                  return a.id < b.id;
              });
    std::sort(snap.lines.begin(), snap.lines.end(),
              [](const LineSnapshot& a, const LineSnapshot& b) {
                  return a.name < b.name;
              });

    const bool ok = writeFsmSnapshotFile(path, snap);
    if (ok)
    {
        lg2::info("pwrseq-monitor: wrote FSM snapshot to {PATH} "
                  "(elements={N}, modules={M}, lines={L})",
                  "PATH", path,
                  "N", static_cast<int>(snap.elements.size()),
                  "M", static_cast<int>(snap.modules.size()),
                  "L", static_cast<int>(snap.lines.size()));
    }
    else
    {
        lg2::error("pwrseq-monitor: failed to write FSM snapshot to {PATH}",
                   "PATH", path);
    }
    return ok;
}

std::optional<InstanceId> Orchestrator::instanceByDescriptorId(
    const std::string& id) const
{
    auto it = instanceByDescId_.find(id);
    if (it == instanceByDescId_.end()) return std::nullopt;
    return unpackInstance(it->second);
}

std::string Orchestrator::descriptorIdByInstance(InstanceId id) const
{
    auto it = descIdByInstance_.find(packInstance(id));
    if (it == descIdByInstance_.end()) return {};
    return it->second;
}

std::vector<InstanceId> Orchestrator::presentInstances(
    InstanceId::Class cls) const
{
    // deviceByInstance_ contains exactly the elements whose driver init
    // returned Ok (sparse modules / failed init are absent). That's the
    // right denominator for mass-reset operations -- we only dispatch
    // to elements we believe are functional.
    std::vector<InstanceId> out;
    out.reserve(deviceByInstance_.size());
    for (const auto& [key, _dev] : deviceByInstance_)
    {
        auto id = unpackInstance(key);
        if (id.cls == cls) out.push_back(id);
    }
    // Sort by index for stable iteration order; tests + log readability.
    std::sort(out.begin(), out.end(),
              [](InstanceId a, InstanceId b) { return a.index < b.index; });
    return out;
}

hal::Result Orchestrator::setVrTelemetryConfigured(InstanceId target,
                                                   bool enable)
{
    if (target.cls != InstanceId::Class::Lpu)
    {
        return hal::Result::BadArgument;
    }

    const uint64_t key = packInstance(target);
    const auto devIt = deviceByInstance_.find(key);
    const auto fsmIt = elementByInstance_.find(key);
    if (devIt == deviceByInstance_.end() || fsmIt == elementByInstance_.end())
    {
        return hal::Result::NotPresent;
    }

    const auto configured = devIt->second->setVrTelemetryConfigured(enable);
    if (configured != hal::Result::Ok)
    {
        return configured;
    }

    cancelTelemetryRetry_(target);
    if (fsmIt->second->state() != ElementState::Online)
    {
        lg2::info(
            "LPU {ID} VR telemetry configuration stored as {STATE}; polling remains gated until Online",
            "ID", to_string(target), "STATE", enable ? "lpu" : "direct");
        return hal::Result::Ok;
    }

    const auto polling = devIt->second->setTelemetryPolling(true);
    if (polling == hal::Result::NotReady)
    {
        scheduleTelemetryRetry_(target, 1);
        return hal::Result::Ok;
    }
    return polling;
}

bool Orchestrator::resetEligible_(InstanceId id) const
{
    const auto it = elementByInstance_.find(packInstance(id));
    if (it == elementByInstance_.end())
    {
        return false;
    }
    return it->second->hasSeenPgHi();
}

void Orchestrator::startBootTimer_(InstanceId id)
{
    sd_event_source* src = nullptr;
    uint64_t now = 0;
    sd_event_now(loop_, CLOCK_MONOTONIC, &now);
    uint64_t at = now + std::chrono::duration_cast<std::chrono::microseconds>(
                            tBoot_).count();

    // ctx lifetime is tied to the sd_event_source via destroy_callback so it
    // is freed regardless of which path tears the source down:
    //   * timer expires            (callback below, then source unref)
    //   * cancelBootTimer_() fires (source unref directly)
    //   * orchestrator destruction (source unref from bootTimers_ cleanup)
    // Previously only the expiry path freed ctx, leaking one allocation per
    // arm/cancel cycle whenever the FSM walked S_POWERED -> S_ONLINE before
    // T_BOOT expired (the common case on a healthy boot).
    struct TimerCtx { Orchestrator* self; InstanceId id; };
    auto* ctx = new TimerCtx{this, id};

    int rc = sd_event_add_time(loop_, &src, CLOCK_MONOTONIC, at, 0,
                      [](sd_event_source* /*s*/, uint64_t,
                         void* userdata) -> int {
                          auto* c = static_cast<TimerCtx*>(userdata);
                          auto it = c->self->elementByInstance_.find(
                              packInstance(c->id));
                          if (it != c->self->elementByInstance_.end())
                          {
                              // Sticky BC (FPGA CONF_DONE across N_CONFIG):
                              // no rising edge — re-read gpio-mon before
                              // BOOT_TIMEOUT. Inject → BcHi → S_ONLINE.
                              if (!c->self->tryRecoverStickyBc_(c->id))
                              {
                                  it->second->dispatch(SemanticEvent{
                                      EventKind::TBootExpired, c->id});
                              }
                          }
                          c->self->bootTimers_.erase(packInstance(c->id));
                          return 0;  // destroy_callback frees ctx
                      },
                      ctx);
    if (rc < 0)
    {
        delete ctx;
        lg2::error("startBootTimer_: sd_event_add_time failed for {ID}: {ERR}",
                   "ID", to_string(id), "ERR", std::strerror(-rc));
        return;
    }
    sd_event_source_set_destroy_callback(src, +[](void* userdata) {
        delete static_cast<TimerCtx*>(userdata);
    });
    bootTimers_[packInstance(id)] = BootTimer{src, id};
}

void Orchestrator::cancelBootTimer_(InstanceId id)
{
    auto it = bootTimers_.find(packInstance(id));
    if (it == bootTimers_.end()) return;
    if (it->second.source)
    {
        sd_event_source_set_enabled(it->second.source, SD_EVENT_OFF);
        sd_event_source_unref(it->second.source);
    }
    bootTimers_.erase(it);
}

void Orchestrator::scheduleTelemetryRetry_(InstanceId id, unsigned attempt)
{
    cancelTelemetryRetry_(id);

    uint64_t now = 0;
    sd_event_now(loop_, CLOCK_MONOTONIC, &now);
    const uint64_t at =
        now + std::chrono::duration_cast<std::chrono::microseconds>(
                  kTelemetryRetryInterval)
                  .count();

    struct TimerCtx
    {
        Orchestrator* self;
        InstanceId id;
        unsigned attempt;
    };
    auto* ctx = new TimerCtx{this, id, attempt};

    sd_event_source* src = nullptr;
    const int rc = sd_event_add_time(
        loop_, &src, CLOCK_MONOTONIC, at, 0,
        [](sd_event_source* source, uint64_t, void* userdata) -> int {
            auto* context = static_cast<TimerCtx*>(userdata);
            Orchestrator* self = context->self;
            const InstanceId id = context->id;
            const unsigned attempt = context->attempt;
            self->telemetryRetryTimers_.erase(packInstance(id));
            self->handleTelemetryRetry_(id, attempt);
            sd_event_source_unref(source);
            return 0;
        },
        ctx);
    if (rc < 0)
    {
        delete ctx;
        lg2::error(
            "scheduleTelemetryRetry_: sd_event_add_time failed for {ID}: {ERR}",
            "ID", to_string(id), "ERR", std::strerror(-rc));
        return;
    }
    sd_event_source_set_destroy_callback(
        src, +[](void* userdata) { delete static_cast<TimerCtx*>(userdata); });
    telemetryRetryTimers_[packInstance(id)] =
        TelemetryRetryTimer{src, id, attempt};
}

void Orchestrator::cancelTelemetryRetry_(InstanceId id)
{
    auto it = telemetryRetryTimers_.find(packInstance(id));
    if (it == telemetryRetryTimers_.end())
        return;
    if (it->second.source)
    {
        sd_event_source_set_enabled(it->second.source, SD_EVENT_OFF);
        sd_event_source_unref(it->second.source);
    }
    telemetryRetryTimers_.erase(it);
}

void Orchestrator::handleTelemetryRetry_(InstanceId id, unsigned attempt)
{
    const uint64_t key = packInstance(id);
    const auto fsmIt = elementByInstance_.find(key);
    const auto devIt = deviceByInstance_.find(key);
    if (fsmIt == elementByInstance_.end() || devIt == deviceByInstance_.end() ||
        fsmIt->second->state() != ElementState::Online)
    {
        return;
    }

    const auto result = devIt->second->setTelemetryPolling(true);
    if (result == hal::Result::NotReady)
    {
        if (attempt < kMaxTelemetryRetryAttempts)
        {
            scheduleTelemetryRetry_(id, attempt + 1);
        }
        else
        {
            lg2::warning(
                "LPU {ID} firmware version remained unavailable after {N} retries; VR polling stays disabled",
                "ID", to_string(id), "N", attempt);
        }
        return;
    }

    if (result != hal::Result::Ok && result != hal::Result::NotSupported)
    {
        lg2::warning("LPU {ID} telemetry retry failed ({RC})", "ID",
                     to_string(id), "RC", std::string(hal::to_string(result)));
    }
}

bool Orchestrator::tryRecoverStickyBc_(InstanceId id)
{
    auto dit = deviceByInstance_.find(packInstance(id));
    if (dit == deviceByInstance_.end() || dit->second == nullptr)
    {
        return false;
    }
    hal::IDevice* dev = dit->second;
    const auto lineName = std::string(dev->bootCompleteLineName());
    if (lineName.empty())
    {
        return false;
    }

    // A level no edge announced, so the SharedLine cache cannot answer it:
    // the pin may have stayed asserted straight through the reset pulse.
    const std::optional<bool> electrical =
        gpio_mon::readLineLevel(bus_, lineName);
    if (!electrical)
    {
        lg2::warning("tryRecoverStickyBc_: gpio-mon read failed for {ID} "
                     "line={LINE}",
                     "ID", to_string(id), "LINE", lineName);
        return false;
    }

    const bool recovered = hal::tryRecoverStickyBootComplete(
        electrical, dev->bootCompleteActiveLow(),
        [dev](bool elec) {
            return dev->injectBootCompleteElectrical(elec);
        });

    if (recovered)
    {
        lg2::info("tryRecoverStickyBc_: {ID} BC still asserted on gpio-mon "
                  "({LINE}={ELEC}); synthesizing BcHi instead of "
                  "BOOT_TIMEOUT/Timeout",
                  "ID", to_string(id), "LINE", lineName, "ELEC",
                  electrical.value_or(false) ? 1 : 0);
    }
    return recovered;
}

}  // namespace pwrseq
