/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "hal/lpu_driver.hpp"

#include "hal/pg_bc_latch.hpp"

#include <lpu850/lpu850-uapi.h>

#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pwrseq::hal
{

namespace
{
constexpr uint8_t minVrFirmwareMajor = 11;
constexpr uint8_t minVrFirmwareMinor = 3;
constexpr uint8_t invalidFirmwareByte = 0xff;

bool paramFlag(const DeviceConfig& cfg, std::string_view key)
{
    auto v = cfg.param(key);
    return v == "1" || v == "true";
}
}  // namespace

Result LpuDriver::probe()
{
    return Result::Ok;
}

Result LpuDriver::init(SemanticEventSink& sink, const DeviceConfig& cfg)
{
    if (sink_ != nullptr || pgLine_ || bcLine_)
    {
        lg2::error("LpuDriver[{ID}]: already initialised; call shutdown() "
                   "before re-init",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    const std::string pgLineName    = cfg.param("pg_line");
    const std::string bcLineName    = cfg.param("bc_line");
    const std::string resetLineName = cfg.param("reset_line");

    vrTelemetryConfigured_ = false;
    vrPollingSelected_ = false;

    if (pgLineName.empty() || bcLineName.empty() || resetLineName.empty())
    {
        lg2::error("LpuDriver[{ID}]: missing required driverParams",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    if (cfg.sharedLines == nullptr)
    {
        lg2::error("LpuDriver[{ID}]: DeviceConfig.sharedLines is null; "
                   "orchestrator must inject a SharedLineRegistry",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    if (cfg.resetChips == nullptr)
    {
        lg2::error("LpuDriver[{ID}]: DeviceConfig.resetChips is null; "
                   "orchestrator must inject a ResetChipRegistry",
                   "ID", to_string(id_));
        return Result::BadArgument;
    }

    const common::GpioLine pg{pgLineName, paramFlag(cfg, "pg_active_low")};
    const common::GpioLine bc{bcLineName, paramFlag(cfg, "bc_active_low")};

    std::shared_ptr<common::SharedLine> newPgLine;
    std::shared_ptr<common::SharedLine> newBcLine;
    common::Subscription                newPgSub;
    common::Subscription                newBcSub;

    try
    {
        newPgLine = cfg.sharedLines->acquire(pg, "pwrseq-monitor");
        newBcLine = cfg.sharedLines->acquire(bc, "pwrseq-monitor");

        const auto myId = id_;
        // PG rising samples the BC latch (OOO BC-before-PG / seed); PG
        // falling invalidates BC so the next rise cannot see a stale
        // BOOT_COMPLETE from the previous power cycle (NVBUG 6559782).
        newPgSub = newPgLine->subscribe(
            [this, myId](bool asserted) {
                if (!sink_)
                {
                    return;
                }
                const bool bcHigh =
                    bcLine_ && bcLine_->hasValue() && bcLine_->read();
                emitPowerGoodEdge(*sink_, myId, asserted, bcHigh, [this] {
                    clearBootCompleteCache();
                });
            });
        newBcSub = newBcLine->subscribe(
            [this, myId](bool asserted) {
                if (sink_) sink_->emit(SemanticEvent{
                    asserted ? EventKind::BcHi : EventKind::BcLo, myId});
            });
    }
    catch (const std::exception& e)
    {
        lg2::error("LpuDriver[{ID}] init failed: {ERR}",
                   "ID", to_string(id_), "ERR", e.what());
        return Result::InitFailed;
    }

    sink_           = &sink;
    cfg_            = cfg;
    pgLineName_     = pgLineName;
    bcLineName_     = bcLineName;
    resetLineName_  = resetLineName;
    resetActiveLow_ = paramFlag(cfg, "reset_active_low");
    pgLine_         = std::move(newPgLine);
    bcLine_         = std::move(newBcLine);
    pgSub_          = std::move(newPgSub);
    bcSub_          = std::move(newBcSub);

    caps_.powerGood    = true;
    caps_.bootComplete = true;
    caps_.warmReset    = true;
    caps_.powerCycle   = false;

    const auto resetChipPath = cfg.resetChips->probeOrStore(resetLineName_);
    if (resetChipPath.empty())
    {
        lg2::info("LpuDriver[{ID}]: init ok (reset_line={LINE}; chip not "
                  "present yet, will resolve on first reset)",
                  "ID", to_string(id_), "LINE", resetLineName_);
    }
    else
    {
        lg2::info("LpuDriver[{ID}]: init ok (reset_line={LINE}, chip={CHIP} "
                  "cached; line requested on demand)",
                  "ID", to_string(id_), "LINE", resetLineName_, "CHIP",
                  resetChipPath);
    }

    const std::string logDev = cfg.param("log_dev");
    if (!logDev.empty())
    {
        logFd_ = ::open(logDev.c_str(), O_RDWR | O_CLOEXEC);
        if (logFd_ < 0)
        {
            lg2::info("LpuDriver[{ID}]: log_dev {DEV} not available ({ERR}); "
                      "telemetry polling control disabled",
                      "ID", to_string(id_), "DEV", logDev,
                      "ERR", std::strerror(errno));
        }
        else
        {
            lg2::info("LpuDriver[{ID}]: log_dev {DEV} open for polling control",
                      "ID", to_string(id_), "DEV", logDev);
            vrPollingSelected_ = false;
            static_cast<void>(applyTelemetryPolling(false));
        }
    }

    return Result::Ok;
}

Result LpuDriver::reset(ResetKind kind)
{
    if (kind != ResetKind::Warm)
    {
        return Result::NotSupported;
    }

    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    lg2::info("LpuDriver[{ID}]: warm-reset trigger on {LINE}",
              "ID", to_string(id_), "LINE", resetLineName_);
    return common::actuateResetOutputLine(resetLineName_, resetActiveLow_,
                                          "pwrseq-monitor",
                                          common::ResetLineOp::Trigger,
                                          cfg_.resetChips);
}

Result LpuDriver::resetAssert(ResetKind kind)
{
    if (kind != ResetKind::Warm)
    {
        return Result::NotSupported;
    }
    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    lg2::info("LpuDriver[{ID}]: warm-reset assert on {LINE} (LSTP trigger)",
              "ID", to_string(id_), "LINE", resetLineName_);
    return common::actuateResetOutputLine(resetLineName_, resetActiveLow_,
                                          "pwrseq-monitor",
                                          common::ResetLineOp::AssertOnly,
                                          cfg_.resetChips);
}

Result LpuDriver::resetRelease()
{
    if (resetLineName_.empty())
    {
        return Result::NotSupported;
    }

    // LP30 LSTP path does not hold reset outputs across calls; release is a
    // no-op when nothing is actively requested.
    lg2::info("LpuDriver[{ID}]: warm-reset release on {LINE} (no-op on LSTP)",
              "ID", to_string(id_), "LINE", resetLineName_);
    return Result::Ok;
}

LpuDriver::VrPollingDecision LpuDriver::selectLpuVrPolling()
{
    if (!vrTelemetryConfigured_ || logFd_ < 0)
    {
        return VrPollingDecision::Disabled;
    }

    lpu_fw_version version{};
    if (::ioctl(logFd_, LPU_LOG_GET_FW_VERSION, &version) < 0)
    {
        lg2::info(
            "LpuDriver[{ID}]: VR polling decision deferred; firmware version unavailable ({ERR})",
            "ID", to_string(id_), "ERR", std::strerror(errno));
        return VrPollingDecision::Retry;
    }

    if (version.major == invalidFirmwareByte &&
        version.minor == invalidFirmwareByte &&
        version.patch == invalidFirmwareByte)
    {
        lg2::info(
            "LpuDriver[{ID}]: VR polling decision deferred; firmware version returned the all-ones unavailable value",
            "ID", to_string(id_));
        return VrPollingDecision::Retry;
    }

    const bool firmwareSupported =
        version.major > minVrFirmwareMajor ||
        (version.major == minVrFirmwareMajor &&
         version.minor >= minVrFirmwareMinor);
    if (!firmwareSupported)
    {
        lg2::info(
            "LpuDriver[{ID}]: VR polling disabled; firmware {MAJOR}.{MINOR}.{PATCH} is older than 11.3.0",
            "ID", to_string(id_), "MAJOR", static_cast<int>(version.major),
            "MINOR", static_cast<int>(version.minor), "PATCH",
            static_cast<int>(version.patch));
        return VrPollingDecision::Disabled;
    }

    lg2::info(
        "LpuDriver[{ID}]: LPU VR polling enabled by platform configuration; firmware={MAJOR}.{MINOR}.{PATCH}",
        "ID", to_string(id_), "MAJOR", static_cast<int>(version.major), "MINOR",
        static_cast<int>(version.minor), "PATCH",
        static_cast<int>(version.patch));
    return VrPollingDecision::Enabled;
}

Result LpuDriver::applyTelemetryPolling(bool enable)
{
    uint8_t mask = enable ? static_cast<uint8_t>(LPU_POLL_ALL) : 0;
    if (enable && vrPollingSelected_)
    {
        mask |= LPU_POLL_VR;
    }

    if (::ioctl(logFd_, LPU_LOG_SET_POLLING, &mask) < 0)
    {
        lg2::warning("LpuDriver[{ID}]: LPU_LOG_SET_POLLING({MASK}) failed: "
                     "{ERR}",
                     "ID", to_string(id_), "MASK", static_cast<int>(mask),
                     "ERR", std::strerror(errno));
        return Result::HwError;
    }

    lg2::info("LpuDriver[{ID}]: telemetry polling {STATE} (mask={MASK})",
              "ID", to_string(id_),
              "STATE", enable ? "enabled" : "disabled",
              "MASK", static_cast<int>(mask));
    return Result::Ok;
}

Result LpuDriver::requestLogDrain(std::string_view reason)
{
    if (logFd_ < 0)
    {
        return Result::NotSupported;
    }
    if (::ioctl(logFd_, LPU_LOG_DRAIN_NOW) < 0)
    {
        lg2::warning("LpuDriver[{ID}]: event log drain failed reason={REASON} "
                     "error={ERR}",
                     "ID", to_string(id_), "REASON", std::string(reason),
                     "ERR", std::strerror(errno));
        return Result::HwError;
    }
    lg2::info("LpuDriver[{ID}]: event log drain requested reason={REASON}",
              "ID", to_string(id_), "REASON", std::string(reason));
    return Result::Ok;
}

Result LpuDriver::setTelemetryPolling(bool enable)
{
    if (logFd_ < 0)
    {
        return Result::NotSupported;
    }

    if (enable)
    {
        const auto decision = selectLpuVrPolling();
        vrPollingSelected_ = decision == VrPollingDecision::Enabled;
        const auto result = applyTelemetryPolling(true);
        if (result != Result::Ok)
        {
            return result;
        }
        return decision == VrPollingDecision::Retry ? Result::NotReady
                                                    : Result::Ok;
    }
    else
    {
        vrPollingSelected_ = false;
    }
    return applyTelemetryPolling(enable);
}

Result LpuDriver::setVrTelemetryConfigured(bool enable)
{
    vrTelemetryConfigured_ = enable;
    lg2::info("LpuDriver[{ID}]: platform VR telemetry source={SOURCE}", "ID",
              to_string(id_), "SOURCE", enable ? "lpu" : "direct");
    return Result::Ok;
}

Result LpuDriver::shutdown()
{
    if (logFd_ >= 0)
    {
        vrPollingSelected_ = false;
        static_cast<void>(applyTelemetryPolling(false));
        ::close(logFd_);
        logFd_ = -1;
    }
    pgSub_.reset();
    bcSub_.reset();
    pgLine_.reset();
    bcLine_.reset();
    vrTelemetryConfigured_ = false;
    vrPollingSelected_ = false;
    return Result::Ok;
}

void LpuDriver::clearBootCompleteCache()
{
    if (bcLine_)
    {
        bcLine_->setCachedSilently(false);
    }
}

std::string_view LpuDriver::bootCompleteLineName() const noexcept
{
    return bcLineName_;
}

bool LpuDriver::bootCompleteActiveLow() const noexcept
{
    return bcLine_ ? bcLine_->activeLow() : false;
}

bool LpuDriver::injectBootCompleteElectrical(bool electricalAsserted)
{
    if (!bcLine_)
    {
        return false;
    }
    bcLine_->notifyExternal(electricalAsserted);
    return true;
}

DeviceStatus LpuDriver::get_status() const
{
    DeviceStatus s{};
    try
    {
        if (pgLine_) s.powerGood    = pgLine_->read();
        if (bcLine_) s.bootComplete = bcLine_->read();
    }
    catch (...) {}
    s.present = (pgLine_ != nullptr && bcLine_ != nullptr);
    return s;
}

std::vector<int> LpuDriver::fds() const
{
    std::vector<int> out;
    if (pgLine_)
    {
        const int fd = pgLine_->fd();
        if (fd >= 0) out.push_back(fd);
    }
    if (bcLine_)
    {
        const int fd = bcLine_->fd();
        if (fd >= 0) out.push_back(fd);
    }
    return out;
}

void LpuDriver::onReadable(int /*fd*/)
{
    // Input edges arrive via Gpio.Monitor.Line PropertiesChanged; no
    // kernel fds are held.
}

}  // namespace pwrseq::hal
