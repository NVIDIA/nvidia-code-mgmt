/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "hal/driver_common.hpp"
#include "hal/i_device.hpp"

#include <memory>
#include <string>
#include <vector>

namespace pwrseq::hal
{

/** HAL driver for a single FPGA instance (×2 on LP30).
 *
 *  PG/BC: gpio-mon Line PropertiesChanged → SharedLine subscribers.
 *  PG rising samples the BC SharedLine latch and may synthesize BcHi when
 *  BC is already asserted (OOO / seed); PG falling invalidates that latch
 *  before emitting PgLo (see hal/pg_bc_latch.hpp).
 *  Reset: on-demand libgpiod assert on reset_line (LSTP output); the
 *  kernel line request is dropped before returning so USB hotplug can
 *  rebind module gpiochips without stale handles.
 */
class FpgaDriver : public IDevice
{
  public:
    explicit FpgaDriver(InstanceId id) noexcept : id_(id) {}

    Result        probe() override;
    Result        init(SemanticEventSink& sink,
                       const DeviceConfig& cfg) override;
    Result        reset(ResetKind kind) override;
    Result        resetAssert(ResetKind kind) override;
    Result        resetRelease() override;
    Result        shutdown() override;
    void          clearBootCompleteCache() override;
    std::string_view bootCompleteLineName() const noexcept override;
    bool          bootCompleteActiveLow() const noexcept override;
    bool          injectBootCompleteElectrical(bool electricalAsserted) override;
    DeviceStatus  get_status() const override;
    Capabilities  capabilities() const override { return caps_; }
    InstanceId    id() const override { return id_; }
    std::vector<int> fds() const override;
    void          onReadable(int fd) override;

  private:
    InstanceId                          id_;
    SemanticEventSink*                  sink_ = nullptr;
    DeviceConfig                        cfg_{};
    Capabilities                        caps_{};

    std::shared_ptr<common::SharedLine> pgLine_;
    std::shared_ptr<common::SharedLine> bcLine_;
    common::Subscription                pgSub_;
    common::Subscription                bcSub_;
    std::string                         pgLineName_;
    std::string                         bcLineName_;
    std::string                         resetLineName_;
    bool                                resetActiveLow_ = false;
};

}  // namespace pwrseq::hal
