/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "hal/fpga_driver.hpp"

#include "hal/pg_bc_latch.hpp"

#include <phosphor-logging/lg2.hpp>

#include <string>

namespace pwrseq::hal
{

namespace
{
bool paramFlag(const DeviceConfig& cfg, std::string_view key)
{
    auto v = cfg.param(key);
    return v == "1" || v == "true";
}
} // namespace

Result FpgaDriver::probe()
{
    return Result::Ok;
}

Result FpgaDriver::init(SemanticEventSink& sink, const DeviceConfig& cfg)
{
    if (sink_ != nullptr || pgLine_ || bcLine_)
    {
        lg2::error("FpgaDriver[{ID}]: already initialised; call shutdown() "
                   "before re-init",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    pgLineName_ = cfg.param("pg_line");
    bcLineName_ = cfg.param("bc_line");
    resetLineName_ = cfg.param("reset_line");

    if (pgLineName_.empty() || bcLineName_.empty() || resetLineName_.empty())
    {
        lg2::error("FpgaDriver[{ID}]: missing required driverParams", "ID",
                   to_string(id_));
        return Result::BadArgument;
    }

    if (cfg.sharedLines == nullptr)
    {
        lg2::error("FpgaDriver[{ID}]: DeviceConfig.sharedLines is null; "
                   "orchestrator must inject a SharedLineRegistry",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    if (cfg.resetChips == nullptr)
    {
        lg2::error("FpgaDriver[{ID}]: DeviceConfig.resetChips is null; "
                   "orchestrator must inject a ResetChipRegistry",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    const common::GpioLine pg{pgLineName_, paramFlag(cfg, "pg_active_low")};
    const common::GpioLine bc{bcLineName_, paramFlag(cfg, "bc_active_low")};

    std::shared_ptr<common::SharedLine> newPgLine;
    std::shared_ptr<common::SharedLine> newBcLine;
    common::Subscription newPgSub;
    common::Subscription newBcSub;
    try
    {
        newPgLine = cfg.sharedLines->acquire(pg, "pwrseq-monitor");
        newBcLine = cfg.sharedLines->acquire(bc, "pwrseq-monitor");

        const auto myId = id_;
        // PG rising samples the BC latch (OOO BC-before-PG / seed); PG
        // falling invalidates BC so the next rise cannot see a stale
        // BOOT_COMPLETE from the previous power cycle (NVBUG 6559782).
        newPgSub = newPgLine->subscribe([this, myId](bool asserted) {
            if (!sink_)
            {
                return;
            }
            const bool bcHigh =
                bcLine_ && bcLine_->hasValue() && bcLine_->read();
            emitPowerGoodEdge(*sink_, myId, asserted, bcHigh,
                              [this] { clearBootCompleteCache(); });
        });
        newBcSub = newBcLine->subscribe([this, myId](bool asserted) {
            if (sink_)
                sink_->emit(SemanticEvent{
                    asserted ? EventKind::BcHi : EventKind::BcLo, myId});
        });
    }
    catch (const std::exception& e)
    {
        lg2::error("FpgaDriver[{ID}] init failed: {ERR}", "ID", to_string(id_),
                   "ERR", e.what());
        return Result::InitFailed;
    }

    sink_ = &sink;
    cfg_ = cfg;
    resetActiveLow_ = paramFlag(cfg, "reset_active_low");
    pgLine_ = std::move(newPgLine);
    bcLine_ = std::move(newBcLine);
    pgSub_ = std::move(newPgSub);
    bcSub_ = std::move(newBcSub);

    caps_.powerGood = true;
    caps_.bootComplete = true;
    caps_.warmReset = true;
    caps_.coldReset = true;
    caps_.powerCycle = true;
    caps_.perRailHealth = false;

    const auto resetChipPath = cfg.resetChips->probeOrStore(resetLineName_);
    if (resetChipPath.empty())
    {
        lg2::info("FpgaDriver[{ID}]: init ok (reset_line={LINE}; chip not "
                  "present yet, will resolve on first reset)",
                  "ID", to_string(id_), "LINE", resetLineName_);
    }
    else
    {
        lg2::info("FpgaDriver[{ID}]: init ok (reset_line={LINE}, chip={CHIP} "
                  "cached; line requested on demand)",
                  "ID", to_string(id_), "LINE", resetLineName_, "CHIP",
                  resetChipPath);
    }

    return Result::Ok;
}

Result FpgaDriver::reset(ResetKind kind)
{
    if (kind != ResetKind::Warm && kind != ResetKind::Cold &&
        kind != ResetKind::PowerCycle)
    {
        return Result::NotSupported;
    }

    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    lg2::info("FpgaDriver[{ID}]: warm-reset trigger on {LINE} (kind={KIND})",
              "ID", to_string(id_), "LINE", resetLineName_, "KIND",
              static_cast<int>(kind));
    return common::actuateResetOutputLine(
        resetLineName_, resetActiveLow_, "pwrseq-monitor",
        common::ResetLineOp::Trigger, cfg_.resetChips);
}

Result FpgaDriver::resetAssert(ResetKind kind)
{
    if (kind != ResetKind::Warm && kind != ResetKind::Cold &&
        kind != ResetKind::PowerCycle)
    {
        return Result::NotSupported;
    }
    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    lg2::info("FpgaDriver[{ID}]: warm-reset assert on {LINE} "
              "(LSTP trigger, kind={KIND})",
              "ID", to_string(id_), "LINE", resetLineName_, "KIND",
              static_cast<int>(kind));
    return common::actuateResetOutputLine(
        resetLineName_, resetActiveLow_, "pwrseq-monitor",
        common::ResetLineOp::AssertOnly, cfg_.resetChips);
}

Result FpgaDriver::resetRelease()
{
    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    lg2::info("FpgaDriver[{ID}]: warm-reset release on {LINE} (no-op on LSTP)",
              "ID", to_string(id_), "LINE", resetLineName_);
    return Result::Ok;
}

Result FpgaDriver::shutdown()
{
    pgSub_.reset();
    bcSub_.reset();
    pgLine_.reset();
    bcLine_.reset();
    return Result::Ok;
}

void FpgaDriver::clearBootCompleteCache()
{
    if (bcLine_)
    {
        bcLine_->setCachedSilently(false);
    }
}

std::string_view FpgaDriver::bootCompleteLineName() const noexcept
{
    return bcLineName_;
}

bool FpgaDriver::bootCompleteActiveLow() const noexcept
{
    return bcLine_ ? bcLine_->activeLow() : false;
}

bool FpgaDriver::injectBootCompleteElectrical(bool electricalAsserted)
{
    if (!bcLine_)
    {
        return false;
    }
    bcLine_->notifyExternal(electricalAsserted);
    return true;
}

DeviceStatus FpgaDriver::get_status() const
{
    DeviceStatus s{};
    try
    {
        if (pgLine_)
            s.powerGood = pgLine_->read();
        if (bcLine_)
            s.bootComplete = bcLine_->read();
    }
    catch (...)
    {}
    s.present = (pgLine_ != nullptr && bcLine_ != nullptr);
    return s;
}

std::vector<int> FpgaDriver::fds() const
{
    std::vector<int> out;
    if (pgLine_)
    {
        const int fd = pgLine_->fd();
        if (fd >= 0)
            out.push_back(fd);
    }
    if (bcLine_)
    {
        const int fd = bcLine_->fd();
        if (fd >= 0)
            out.push_back(fd);
    }
    return out;
}

void FpgaDriver::onReadable(int /*fd*/)
{
    // Input edges arrive via Gpio.Monitor.Line PropertiesChanged; no
    // kernel fds are held.
}

} // namespace pwrseq::hal
