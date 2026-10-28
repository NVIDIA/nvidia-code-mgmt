/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * In-memory mock IDevice + SemanticEventSink for unit tests. Lets tests
 * drive the FSM engine without touching real hardware, libgpiod, or any
 * system file descriptors.
 */

#pragma once

#include "core/types.hpp"
#include "hal/i_device.hpp"

#include <vector>

namespace pwrseq::hal::test
{

/** Recording SemanticEventSink — tests inspect `events` after each step. */
class RecordingSink : public SemanticEventSink
{
  public:
    void emit(const SemanticEvent& ev) override
    {
        events.push_back(ev);
    }

    std::vector<SemanticEvent> events;
};

/** MockDevice — fully programmable IDevice for tests.
 *  - probe() returns whatever you set in probeResult.
 *  - init() captures the sink so the test can use mock.emit(...) to inject
 *    synthetic edges.
 *  - reset() records the call and returns resetResult.
 *  - fds() returns empty (mock has no real file descriptors).
 */
class MockDevice : public IDevice
{
  public:
    explicit MockDevice(InstanceId id) : instanceId_(id)
    {}

    Result probe() override
    {
        return probeResult;
    }

    Result init(SemanticEventSink& sink, const DeviceConfig& cfg) override
    {
        sink_ = &sink;
        config_ = cfg;
        return initResult;
    }

    Result reset(ResetKind kind) override
    {
        resetCalls.push_back(kind);
        return resetResult;
    }

    Result shutdown() override
    {
        shutdownCalls++;
        return Result::Ok;
    }

    void clearBootCompleteCache() override
    {
        status.bootComplete = false;
    }

    DeviceStatus get_status() const override
    {
        return status;
    }
    Capabilities capabilities() const override
    {
        return caps;
    }
    InstanceId id() const override
    {
        return instanceId_;
    }
    std::vector<int> fds() const override
    {
        return {};
    }
    void onReadable(int) override
    {}

    /** Test helper: emit a SemanticEvent through the sink as if observed
     *  from hardware. No-op if init() hasn't been called yet.
     */
    void emit(EventKind k)
    {
        if (sink_)
            sink_->emit(SemanticEvent{k, instanceId_});
    }
    void emit(const SemanticEvent& ev)
    {
        if (sink_)
            sink_->emit(ev);
    }

    // Tunables — set before exercising
    Result probeResult = Result::Ok;
    Result initResult = Result::Ok;
    Result resetResult = Result::Ok;
    DeviceStatus status{};
    Capabilities caps{};

    // Recorded calls
    std::vector<ResetKind> resetCalls;
    int shutdownCalls = 0;

    // Captured at init
    SemanticEventSink* sink_ = nullptr;
    DeviceConfig config_{};

  private:
    InstanceId instanceId_;
};

} // namespace pwrseq::hal::test
