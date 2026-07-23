/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "force_recovery.hpp"

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <stdexcept>
#include <vector>

// Claim a GPIO output line and hold it open, returning the line handle.
static std::optional<gpiod::line> claimGpio(const std::string& name, int value)
{
    auto line = gpiod::find_line(name);
    if (!line)
    {
        lg2::error("GPIO line not found: {NAME}", "NAME", name);
        return std::nullopt;
    }
    try
    {
        line.request({"usbrcm", gpiod::line_request::DIRECTION_OUTPUT, 0},
                     value);
        return line;
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to set GPIO {NAME}={VAL}: {ERR}", "NAME", name,
                   "VAL", value, "ERR", e.what());
        return std::nullopt;
    }
}

// Drive reset via D-Bus (assert=true) or release (assert=false).
static void dbusReset(bool assert, const RecoveryPinConfig& cfg)
{
    auto bus = sdbusplus::bus::new_default();
    auto m =
        bus.new_method_call(cfg.dbusService.c_str(), cfg.dbusObject.c_str(),
                            cfg.dbusInterface.c_str(), cfg.dbusMethod.c_str());
    m.append(assert);
    bus.call(m);
}

// Assert all reset lines.  For GPIO mode, returns held-open line handles so
// the reset stays asserted across the strap-write window.
static std::vector<gpiod::line> assertReset(const RecoveryPinConfig& cfg)
{
    if (!cfg.resetPins.empty())
    {
        std::vector<gpiod::line> lines;
        for (const auto& name : cfg.resetPins)
        {
            auto line = gpiod::find_line(name);
            if (!line)
                throw std::runtime_error("Reset GPIO not found: " + name);
            line.request({"usbrcm", gpiod::line_request::DIRECTION_OUTPUT, 0},
                         0); // active-low: 0 = reset asserted
            lines.push_back(std::move(line));
        }
        return lines;
    }
    dbusReset(true, cfg);
    return {};
}

// Release all resets. Reset lines are active-low so driving to 1 de-asserts
// them. For D-Bus mode the owning service performs the de-assertion.
static void releaseReset(const RecoveryPinConfig& cfg,
                         std::vector<gpiod::line>& lines)
{
    if (!lines.empty())
    {
        for (auto& line : lines)
        {
            line.set_value(1); // active-low: 1 = reset de-asserted
        }
    }
    else
    {
        dbusReset(false, cfg);
    }
}

void forceRecoveryMode(const RecoveryPinConfig& cfg, nlohmann::json& out)
{
    // Phase 1: assert all resets; hold GPIO lines open across strap writes.
    std::vector<gpiod::line> resetLines;
    try
    {
        resetLines = assertReset(cfg);
    }
    catch (const std::exception& e)
    {
        out = {{"Status", "Failed"}, {"Error", e.what()}};
        return;
    }

    // Phase 2: claim and hold all strap lines at their active values.
    // Holding them open (not releasing immediately) guarantees the values
    // are stable until reset is released in Phase 3, regardless of whether
    // the underlying GPIO driver or expander retains output state on release.
    std::vector<gpiod::line> strapLines;
    for (size_t i = 0; i < cfg.strapPins.size(); ++i)
    {
        auto line = claimGpio(cfg.strapPins[i], cfg.strapActiveValues[i]);
        if (!line)
        {
            lg2::error("Leaving CPUs in reset after strap failure on {PIN}",
                       "PIN", cfg.strapPins[i]);
            out = {{"Status", "Failed"},
                   {"Error", "Strap GPIO failed: " + cfg.strapPins[i]}};
            return;
        }
        strapLines.push_back(std::move(*line));
    }

    // Phase 3: release all resets; CPUs now sample the held strap values.
    try
    {
        releaseReset(cfg, resetLines);
    }
    catch (const std::exception& e)
    {
        out = {{"Status", "Failed"},
               {"Error", std::string("Reset release failed: ") + e.what()}};
        return;
    }

    // Phase 4: release strap lines — values no longer need to be stable.
    // strapLines destructor handles this automatically on return.

    out = {{"Status", "Successful"}};
}

void setGPIODefaultPinStates(const RecoveryPinConfig& cfg, nlohmann::json& out)
{
    // Attempt all restores regardless of individual failures: a single
    // missing GPIO (e.g. BRD1 not present) must not prevent the remaining
    // straps from being restored to safe defaults.
    std::vector<std::string> failures;
    for (size_t i = 0; i < cfg.strapPins.size(); ++i)
    {
        auto line = claimGpio(cfg.strapPins[i], cfg.strapDefaultValues[i]);
        if (!line)
            failures.push_back(cfg.strapPins[i]);
        // line destructor releases claim; output value is retained by hardware
        // but we do not depend on that — restore is a best-effort cleanup.
    }
    if (!failures.empty())
    {
        std::string pins;
        for (const auto& p : failures)
            pins += (pins.empty() ? "" : ", ") + p;
        out = {{"Status", "Failed"}, {"Error", "GPIO restore failed: " + pins}};
        return;
    }
    out = {{"Status", "Successful"}};
}
