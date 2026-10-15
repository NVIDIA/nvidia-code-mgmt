/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "hal/driver_common.hpp"

#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace pwrseq::hal::common
{

std::string findChipForLine(const std::string& lineName)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists("/dev", ec))
    {
        return {};
    }

    // GPIO line names are NOT globally unique across gpiochips. If two
    // chips expose a line with the same name we must surface that as an
    // error rather than silently bind to whichever appears first in
    // directory iteration order (which is itself unspecified). Scan
    // every chip, accumulate the matches, and let the caller decide:
    //   * zero matches    -> return {} (line not present anywhere)
    //   * one match       -> return that chip path
    //   * two+ matches    -> throw -- the descriptor needs to disambiguate
    //                        (typically by adding an explicit chip path or
    //                        renaming the line on one of the chips).
    std::vector<std::string> matches;

    for (const auto& entry : fs::directory_iterator("/dev", ec))
    {
        if (ec)
        {
            break;
        }
        const auto& path = entry.path();
        const auto& name = path.filename().string();
        if (name.rfind("gpiochip", 0) != 0)
        {
            continue;
        }

        try
        {
            ::gpiod::chip chip(path.string());
            // libgpiodcxx 1.x: find_line() returns a default-constructed
            // (invalid) line when the name isn't on this chip; check via
            // operator bool().
            if (chip.find_line(lineName))
            {
                matches.push_back(path.string());
            }
        }
        catch (const std::exception&)
        {
            // Skip chips we can't open (permissions, busy, etc.).
            continue;
        }
    }

    if (matches.empty()) return {};
    if (matches.size() == 1) return matches.front();

    std::string joined;
    for (const auto& m : matches)
    {
        if (!joined.empty()) joined += ", ";
        joined += m;
    }
    throw std::runtime_error(
        "findChipForLine: line name '" + lineName + "' is ambiguous; "
        "matches on " + std::to_string(matches.size()) + " chips: [" +
        joined + "]. Descriptor must disambiguate (rename the line on "
        "one of the chips, or pin the chip path in platform.json).");
}

// ── OutputLine ────────────────────────────────────────────────────────────

OutputLine::OutputLine(const std::string& chip_path,
                       const GpioLine& gl,
                       const std::string& consumer)
    : chip_(chip_path), name_(gl.name), active_low_(gl.activeLow)
{
    line_ = chip_.find_line(gl.name);
    if (!line_)
    {
        throw std::runtime_error(
            "OutputLine: line '" + gl.name + "' not found on " + chip_path);
    }

    // libgpiodcxx 1.x: request_output() takes consumer name + initial value.
    // We pass the RELEASED electrical value so the daemon coming up does
    // not stall the device. EBUSY here means another process (or another
    // driver in this process) has already requested the line -- that's a
    // configuration error worth surfacing as InitFailed in the caller.
    ::gpiod::line_request req;
    req.consumer     = consumer;
    req.request_type = ::gpiod::line_request::DIRECTION_OUTPUT;
    // Intentionally do NOT set FLAG_ACTIVE_LOW. We track polarity in this
    // class so reads/writes via gpioget(1) from outside (debug) match what
    // the daemon is doing on the wire.
    line_.request(req, electricalValueForRelease_());
    asserted_ = false;  // matches the initial drive value above
}

void OutputLine::assert_()
{
    line_.set_value(electricalValueForAssert_());
    asserted_ = true;
}

void OutputLine::release()
{
    line_.set_value(electricalValueForRelease_());
    asserted_ = false;
}

void OutputLine::trigger()
{
    assert_();
}

std::string probeChipForLine(const std::string& lineName)
{
    return findChipForLine(lineName);
}

// ── ResetChipRegistry ─────────────────────────────────────────────────────

std::string ResetChipRegistry::get(const std::string& lineName) const
{
    const auto it = lines_.find(lineName);
    if (it == lines_.end()) return {};
    return it->second;
}

std::string ResetChipRegistry::probeOrStore(const std::string& lineName)
{
    if (const auto cached = get(lineName); !cached.empty())
    {
        return cached;
    }

    const auto chip = findChipForLine(lineName);
    if (!chip.empty())
    {
        lines_[lineName] = chip;
    }
    return chip;
}

void ResetChipRegistry::store(const std::string& lineName,
                              const std::string& chipPath)
{
    if (chipPath.empty())
    {
        invalidateLine(lineName);
        return;
    }
    lines_[lineName] = chipPath;
}

void ResetChipRegistry::invalidateLine(const std::string& lineName)
{
    lines_.erase(lineName);
}

void ResetChipRegistry::invalidateChip(const std::string& chipPath)
{
    for (auto it = lines_.begin(); it != lines_.end();)
    {
        if (it->second == chipPath)
        {
            it = lines_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

namespace
{

void driveResetLine(OutputLine& line, ResetLineOp op)
{
    switch (op)
    {
        case ResetLineOp::Trigger:
            line.trigger();
            break;
        case ResetLineOp::AssertOnly:
            line.assert_();
            break;
        case ResetLineOp::ReleaseElectrical:
            line.release();
            break;
    }
}

hal::Result actuateOnChip(const std::string& chip, const GpioLine& gl,
                          const std::string& consumer, ResetLineOp op)
{
    OutputLine line(chip, gl, consumer);
    driveResetLine(line, op);
    return hal::Result::Ok;
}

}  // namespace

Result actuateResetOutputLine(const std::string& lineName, bool activeLow,
                              const std::string& consumer, ResetLineOp op,
                              ResetChipRegistry* chips)
{
    const GpioLine gl{lineName, activeLow};
    std::string staleChip;

    if (chips != nullptr)
    {
        const auto cached = chips->get(lineName);
        if (!cached.empty())
        {
            try
            {
                return actuateOnChip(cached, gl, consumer, op);
            }
            catch (const std::exception& e)
            {
                lg2::warning(
                    "actuateResetOutputLine: cached chip {CHIP} stale for "
                    "{LINE} ({ERR}); invalidating chip-wide and rescanning",
                    "CHIP", cached, "LINE", lineName, "ERR", e.what());
                staleChip = cached;
                chips->invalidateChip(cached);
            }
        }
    }

    const auto chip = findChipForLine(lineName);
    if (chip.empty())
    {
        if (chips != nullptr)
        {
            chips->invalidateLine(lineName);
        }
        return hal::Result::Absent;
    }

    try
    {
        auto result = actuateOnChip(chip, gl, consumer, op);
        if (chips != nullptr)
        {
            chips->store(lineName, chip);
        }
        if (!staleChip.empty())
        {
            lg2::info(
                "actuateResetOutputLine: rescan for {LINE} found {CHIP} "
                "(was {STALE}); cache updated and pulse succeeded",
                "LINE", lineName, "CHIP", chip, "STALE", staleChip);
        }
        return result;
    }
    catch (const std::exception& e)
    {
        lg2::error("actuateResetOutputLine: {LINE} on {CHIP} failed: {ERR}",
                   "LINE", lineName, "CHIP", chip, "ERR", e.what());
        return hal::Result::HwError;
    }
}

// ── SharedLine ────────────────────────────────────────────────────────────

SharedLine::SharedLine(PrivateTag, const GpioLine& gl, bool initialAsserted,
                       const std::string& consumer)
    : source_(gl.name, gl.activeLow, initialAsserted),
      consumer_(consumer)
{}

bool SharedLine::read() const
{
    return source_.read();
}

bool SharedLine::hasValue() const noexcept
{
    return source_.hasValue();
}

const std::string& SharedLine::name() const noexcept
{
    return source_.name();
}

bool SharedLine::activeLow() const noexcept
{
    return source_.activeLow();
}

const std::string& SharedLine::consumer() const noexcept
{
    return consumer_;
}

void SharedLine::notifyExternal(bool asserted)
{
    const bool logical = source_.activeLow() ? !asserted : asserted;
    source_.setCached(logical);
    subs_.notify(logical);
}

Subscription SharedLine::subscribe(LineSubscriber cb)
{
    const std::uint64_t slot = subs_.add(std::move(cb));
    return Subscription{weak_from_this(), slot};
}

void SharedLine::unsubscribe(std::uint64_t slot) noexcept
{
    subs_.remove(slot);
}

// ── Subscription ──────────────────────────────────────────────────────────

void Subscription::reset() noexcept
{
    if (!valid_) return;
    valid_ = false;
    if (auto sp = line_.lock())
    {
        sp->unsubscribe(slot_);
    }
    // SharedLine already gone (refcount hit zero before us) — nothing to do.
}

// ── SharedLineRegistry ────────────────────────────────────────────────────

namespace
{

void warnIfDescriptorMismatch(const SharedLine& existing,
                              const GpioLine& gl,
                              const std::string& consumer)
{
    if (existing.activeLow() != gl.activeLow)
    {
        throw std::runtime_error(
            "SharedLineRegistry::acquire: descriptor mismatch for '" +
            gl.name + "' -- existing registration has active_low=" +
            (existing.activeLow() ? "true" : "false") +
            " but caller requested active_low=" +
            (gl.activeLow ? "true" : "false") +
            ". Polarity differences across descriptor entries for the "
            "same line are not supported.");
    }
    if (existing.consumer() != consumer)
    {
        lg2::warning(
            "SharedLineRegistry::acquire: consumer mismatch for "
            "'{LINE}' -- existing registration uses '{HAVE}', caller "
            "requested '{WANT}'. Reusing existing registration; the first "
            "consumer label wins.",
            "LINE", gl.name, "HAVE", existing.consumer(), "WANT", consumer);
    }
}

}  // namespace

std::shared_ptr<SharedLine> SharedLineRegistry::acquire(
    const GpioLine& gl,
    const std::string& consumer)
{
    const Key k{gl.name};
    auto it = lines_.find(k);
    if (it != lines_.end())
    {
        if (auto sp = it->second.lock())
        {
            warnIfDescriptorMismatch(*sp, gl, consumer);
            return sp;
        }
    }

    // PG/BC logical state is unknown until phosphor-multi-gpio-monitor
    // binds the line and publishes Value (PropertiesChanged / snapshot).
    constexpr bool initialAsserted = false;

    auto sp = std::make_shared<SharedLine>(
        SharedLine::PrivateTag{}, gl, initialAsserted, consumer);
    lines_[k] = sp;
    return sp;
}

std::shared_ptr<SharedLine> SharedLineRegistry::findByName(
    const std::string& lineName) const
{
    // Walk the map -- N is small (~30) and this is called on the
    // property-inject path. If profiling ever shows this dominating,
    // switch to a secondary name -> weak_ptr index built up lazily
    // during acquire.
    for (const auto& [_, w] : lines_)
    {
        if (auto sp = w.lock())
        {
            if (sp->name() == lineName) return sp;
        }
    }
    return nullptr;
}

std::size_t SharedLineRegistry::live_count() const
{
    std::size_t n = 0;
    for (const auto& [_, w] : lines_)
    {
        if (!w.expired()) ++n;
    }
    return n;
}

}  // namespace pwrseq::hal::common
