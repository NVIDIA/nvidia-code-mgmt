/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "test_rig.hpp"

#include <stdexcept>
#include <string>

namespace pwrseq::test
{

// ─── construction ──────────────────────────────────────────────────────────

TestRig::TestRig(const config::PlatformDescriptor& desc) :
    tBoot_(desc.tBootMs)
{
    // ── allocate LPU element FSMs + MockDevices, one per element ──────────
    // InstanceId::index is zero-based in production (see
    // include/core/types.hpp and Orchestrator::initFromDescriptor at
    // `nextElementIdx = 0`); the harness mirrors that exactly so test
    // InstanceIds match what production allocates from the same
    // descriptor.
    uint16_t nextLpuIdx = 0;
    for (uint16_t mIdx = 0; mIdx < desc.modules.size(); ++mIdx)
    {
        const auto& mod = desc.modules[mIdx];
        InstanceId modInst = moduleId(mIdx);

        // Module needs the two child InstanceIds at construction time —
        // collect them as we walk the elements.
        std::vector<InstanceId> children;
        children.reserve(mod.elements.size());

        for (size_t i = 0; i < mod.elements.size(); ++i)
        {
            InstanceId elInst = lpuId(nextLpuIdx++);
            children.push_back(elInst);

            auto dev = std::make_unique<hal::test::MockDevice>(elInst);
            auto elFsm = std::make_unique<fsm::ElementFsm>(elInst);

            // Init the MockDevice so its `sink_` points at this rig.
            // DeviceConfig is empty — the rig doesn't read it. We only
            // care that the sink wiring works.
            hal::DeviceConfig cfg{};
            cfg.instanceId    = elInst;
            cfg.initTimeoutMs = tBoot_;
            (void)dev->init(*this, cfg);

            deviceByInstance_[key_(elInst)]  = dev.get();
            elementByInstance_[key_(elInst)] = elFsm.get();
            elementToModule_[key_(elInst)]   = key_(modInst);
            elementIsLpu_[key_(elInst)]      = true;

            devices_.push_back(std::move(dev));
            elements_.push_back(std::move(elFsm));
        }

        if (children.size() != 2)
        {
            // The Module FSM hardcodes a 2-element child array (matches
            // LP30's design where each module is exactly 2 LPUs). Until we
            // generalise the FSM, fail fast here.
            throw std::runtime_error(
                "TestRig: module \"" + mod.id +
                "\" has " + std::to_string(children.size()) +
                " elements; ModuleFsm requires exactly 2");
        }

        auto modFsm =
            std::make_unique<fsm::ModuleFsm>(modInst, children[0], children[1]);
        moduleByInstance_[key_(modInst)] = modFsm.get();
        modules_.push_back(std::move(modFsm));
    }

    // ── allocate FPGA element FSMs + MockDevices ──────────────────────────
    for (uint16_t fIdx = 0; fIdx < desc.fpgas.size(); ++fIdx)
    {
        InstanceId elInst = fpgaId(fIdx);

        auto dev = std::make_unique<hal::test::MockDevice>(elInst);
        auto elFsm = std::make_unique<fsm::ElementFsm>(elInst);

        hal::DeviceConfig cfg{};
        cfg.instanceId    = elInst;
        cfg.initTimeoutMs = tBoot_;
        (void)dev->init(*this, cfg);

        deviceByInstance_[key_(elInst)]  = dev.get();
        elementByInstance_[key_(elInst)] = elFsm.get();
        elementIsLpu_[key_(elInst)]      = false;

        devices_.push_back(std::move(dev));
        elements_.push_back(std::move(elFsm));
    }

    // ── tray FSM (sizes itself based on N modules + M FPGAs) ──────────────
    tray_ = std::make_unique<fsm::TrayFsm>(trayId(),
                                            desc.modules.size(),
                                            desc.fpgas.size());

    // ── wire everything ──────────────────────────────────────────────────
    // Modules first, so element hooks (which forward to a parent module)
    // can rely on findModule_() returning non-null.
    for (auto& m : modules_)
    {
        wireModule_(*m);
    }
    for (auto& e : elements_)
    {
        InstanceId id = e->ctx().id;
        if (elementIsLpu_[key_(id)])
        {
            // LPU: its state changes propagate to its parent module.
            wireElement_(*e, InstanceId{InstanceId::Class::Module,
                                        static_cast<uint16_t>(
                                            elementToModule_[key_(id)] &
                                            0xFFFFu)});
        }
        else
        {
            // FPGA: state changes propagate directly to the tray.
            wireElement_(*e, std::nullopt);
        }
    }
    wireTray_();
}

// ─── wiring ────────────────────────────────────────────────────────────────

void TestRig::wireElement_(fsm::ElementFsm& fsm,
                           std::optional<InstanceId> parentModule)
{
    InstanceId selfId = fsm.ctx().id;

    fsm.setStartTBoot([this, selfId]() {
        // Cancel any pre-existing timer first — defensive; the production
        // FSM should always pair start with cancel, but assert nothing
        // here so flaky scenarios fail predictably.
        auto it = bootTimers_.find(key_(selfId));
        if (it != bootTimers_.end() && it->second != 0)
        {
            clock_.cancel(it->second);
        }

        // Schedule a new T_BOOT timer. When it fires, dispatch
        // EV_T_BOOT_EXPIRED to this element's FSM.
        VirtualClock::TimerId tid =
            clock_.schedule(tBoot_, [this, selfId]() {
                bootTimers_[key_(selfId)] = 0;
                dispatchInternal_(
                    SemanticEvent{EventKind::TBootExpired, selfId});
            });
        bootTimers_[key_(selfId)] = tid;
    });

    fsm.setCancelTBoot([this, selfId]() {
        auto it = bootTimers_.find(key_(selfId));
        if (it != bootTimers_.end() && it->second != 0)
        {
            clock_.cancel(it->second);
            it->second = 0;
        }
    });

    fsm.setOnStateChange([this, selfId, parentModule](
                                  ElementState oldS, ElementState newS) {
        trace_.push_back({TransitionRecord::Tier::Element, selfId,
                          static_cast<int>(oldS), static_cast<int>(newS),
                          clock_.now()});

        if (parentModule.has_value())
        {
            // LPU: tell the parent module.
            dispatchInternal_(SemanticEvent{
                EventKind::LpuStateChanged,
                *parentModule,
                ChildElementPayload{selfId, newS},
            });
        }
        else
        {
            // FPGA: tell the tray directly.
            dispatchInternal_(SemanticEvent{
                EventKind::FpgaStateChanged,
                trayId(),
                ChildElementPayload{selfId, newS},
            });
        }
    });
}

void TestRig::wireModule_(fsm::ModuleFsm& fsm)
{
    InstanceId selfId = fsm.ctx().id;

    fsm.setOnStateChange([this, selfId](
                                  ModuleState oldS, ModuleState newS) {
        trace_.push_back({TransitionRecord::Tier::Module, selfId,
                          static_cast<int>(oldS), static_cast<int>(newS),
                          clock_.now()});

        dispatchInternal_(SemanticEvent{
            EventKind::ModuleStateChanged,
            trayId(),
            ChildModulePayload{selfId, newS},
        });
    });
}

void TestRig::wireTray_()
{
    tray_->setOnStateChange([this](TrayState oldS, TrayState newS) {
        trace_.push_back({TransitionRecord::Tier::Tray, trayId(),
                          static_cast<int>(oldS), static_cast<int>(newS),
                          clock_.now()});
    });
}

// ─── stimulus ──────────────────────────────────────────────────────────────

void TestRig::inject(InstanceId target, EventKind kind)
{
    // Validate the target exists before routing -- silent no-ops from
    // dispatchInternal_ on a typo (wrong index, wrong class) would let
    // a buggy test pass without ever exercising the FSM under test.
    if (!targetExists_(target))
    {
        throw std::runtime_error(
            "TestRig::inject: unknown target " + to_string(target));
    }
    dispatchInternal_(SemanticEvent{kind, target});
}

void TestRig::injectAllLpus(EventKind kind)
{
    for (auto& [k, fsm] : elementByInstance_)
    {
        InstanceId id = fsm->ctx().id;
        if (id.cls == InstanceId::Class::Lpu)
        {
            dispatchInternal_(SemanticEvent{kind, id});
        }
    }
}

void TestRig::injectAllFpgas(EventKind kind)
{
    for (auto& [k, fsm] : elementByInstance_)
    {
        InstanceId id = fsm->ctx().id;
        if (id.cls == InstanceId::Class::Fpga)
        {
            dispatchInternal_(SemanticEvent{kind, id});
        }
    }
}

int TestRig::tick(std::chrono::milliseconds delta)
{
    return clock_.advance(delta);
}

void TestRig::requestReset(InstanceId target, hal::ResetKind kind)
{
    // Reject unknown targets so a typo in test setup fails loudly rather
    // than silently dropping the RstAsserted/RstReleased events and the
    // device->reset() call.
    if (!targetExists_(target))
    {
        throw std::runtime_error(
            "TestRig::requestReset: unknown target " + to_string(target));
    }
    // Mirror Orchestrator::requestReset(): invalidate BC latch, assert
    // reset, drive the device, release. The device's reset() only records
    // the call.
    if (auto* d = findDevice_(target))
    {
        d->clearBootCompleteCache();
        dispatchInternal_(SemanticEvent{EventKind::RstAsserted, target});
        (void)d->reset(kind);
        dispatchInternal_(SemanticEvent{EventKind::RstReleased, target});
        return;
    }
    dispatchInternal_(SemanticEvent{EventKind::RstAsserted, target});
    dispatchInternal_(SemanticEvent{EventKind::RstReleased, target});
}

// ─── observation ───────────────────────────────────────────────────────────

ElementState TestRig::elementState(InstanceId id) const
{
    auto* e = findElement_(id);
    if (!e)
    {
        throw std::runtime_error("TestRig::elementState: unknown id");
    }
    return e->state();
}

ModuleState TestRig::moduleState(InstanceId id) const
{
    auto* m = findModule_(id);
    if (!m)
    {
        throw std::runtime_error("TestRig::moduleState: unknown id");
    }
    return m->state();
}

TrayState TestRig::trayState() const
{
    return tray_->state();
}

FaultCode TestRig::elementFault(InstanceId id) const
{
    auto* e = findElement_(id);
    if (!e)
    {
        throw std::runtime_error("TestRig::elementFault: unknown id");
    }
    return e->faultCode();
}

int TestRig::resetCallCount(InstanceId id) const
{
    auto* d = findDevice_(id);
    return d ? static_cast<int>(d->resetCalls.size()) : 0;
}

const std::vector<hal::ResetKind>& TestRig::resetHistory(InstanceId id) const
{
    auto* d = findDevice_(id);
    if (!d)
    {
        throw std::runtime_error("TestRig::resetHistory: unknown id");
    }
    return d->resetCalls;
}

// ─── SemanticEventSink ─────────────────────────────────────────────────────

void TestRig::emit(const SemanticEvent& ev)
{
    // Entry point used by MockDevice instances when scenarios call
    // dev->emit(...) directly. inject() also funnels through here via
    // dispatchInternal_.
    dispatchInternal_(ev);
}

// ─── routing ───────────────────────────────────────────────────────────────

void TestRig::dispatchInternal_(const SemanticEvent& ev)
{
    switch (ev.target.cls)
    {
        case InstanceId::Class::Lpu:
        case InstanceId::Class::Fpga:
            if (auto* e = findElement_(ev.target))
            {
                e->dispatch(ev);
            }
            break;

        case InstanceId::Class::Module:
            if (auto* m = findModule_(ev.target))
            {
                m->dispatch(ev);
            }
            break;

        case InstanceId::Class::Tray:
            tray_->dispatch(ev);
            break;
    }
}

// ─── lookups ───────────────────────────────────────────────────────────────

bool TestRig::targetExists_(InstanceId id) const
{
    switch (id.cls)
    {
        case InstanceId::Class::Lpu:
        case InstanceId::Class::Fpga:
            return elementByInstance_.contains(key_(id));
        case InstanceId::Class::Module:
            return moduleByInstance_.contains(key_(id));
        case InstanceId::Class::Tray:
            return tray_ != nullptr;
    }
    return false;
}

fsm::ElementFsm* TestRig::findElement_(InstanceId id) const
{
    auto it = elementByInstance_.find(key_(id));
    return it == elementByInstance_.end() ? nullptr : it->second;
}

fsm::ModuleFsm* TestRig::findModule_(InstanceId id) const
{
    auto it = moduleByInstance_.find(key_(id));
    return it == moduleByInstance_.end() ? nullptr : it->second;
}

hal::test::MockDevice* TestRig::findDevice_(InstanceId id) const
{
    auto it = deviceByInstance_.find(key_(id));
    return it == deviceByInstance_.end() ? nullptr : it->second;
}

}  // namespace pwrseq::test
