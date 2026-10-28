/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Platform descriptor — the data model that replaces hardcoded inventory in
 * the orchestrator (design §6, "descriptor-driven instantiation").
 *
 * The descriptor is loaded once at daemon startup from a JSON file (typically
 * /usr/share/lpu-pwrseq-monitor/platform.json). It tells the orchestrator:
 *
 *   - How many modules exist, and which elements (LPU / future MCU / future
 *     GPU) live in each module.
 *   - How many FPGAs exist, with their pg + bc + reset-unit signals.
 *   - The libgpiod line-name (and active-low flag) for every signal each
 *     driver needs. Chip resolution is by line-name search at driver init —
 *     no chip label is baked into the descriptor, so a new platform variant
 *     is a JSON edit and a deploy.
 *
 * --------------------------------------------------------------------------
 * Schema (mirrors the JSON 1:1, see files/platform.json for a worked example):
 *
 *   PlatformDescriptor
 *     ├── platform   : string                (informational, e.g. "LP30-EB2")
 *     ├── chassis_id : string                (Redfish chassis path leaf)
 *     ├── t_boot_ms  : uint                  (per-element boot deadline)
 *     ├── modules : [ModuleDescriptor]
 *     │     ├── id : string                   (e.g. "HGX_LPU_Module_0")
 *     │     └── elements : [ElementDescriptor]
 *     │           ├── kind        : "LPU" | "MCU" | "GPU"
 *     │           ├── id          : string
 *     │           ├── pg          : LineRef
 *     │           ├── bc          : LineRef
 *     │           ├── reset_line  : LineRef             (LSTP reset output;
 *     │           │                                      driver acquires as
 *     │           │                                      OUTPUT and asserts on
 *     │           │                                      reset(Warm).)
 *     │           ├── boot_status_bits : [LineRef x4]   (optional; bit 0..3)
 *     │           └── telemetry   : { log_dev }         (optional; lpu850
 *     │                                                  chardev for per-LPU
 *     │                                                  poll_mask control)
 *     └── fpgas : [FpgaDescriptor]
 *           ├── id          : string
 *           ├── pg          : LineRef
 *           ├── bc          : LineRef                  (FPGA INIT_DONE signal)
 *           ├── reset_line  : LineRef                  (same as element)
 *           └── boot_status_bits : [LineRef x4]        (optional; bit 0..3)
 *
 *   LineRef                                  (always an object)
 *     ├── line       : string                (libgpiod line-name, required)
 *     └── active_low : bool                  (default false)
 *
 *   PG/BC inputs are always monitored by phosphor-multi-gpio-monitor;
 *   edges reach the daemon via Gpio.Monitor.Line PropertiesChanged.
 *
 *   JSON-to-model name mapping (snake_case JSON -> camelCase model):
 *     reset_line  ↔  ElementDescriptor::resetLine, FpgaDescriptor::resetLine
 *     active_low  ↔  LineRef::activeLow
 *     t_boot_ms   ↔  PlatformDescriptor::tBootMs
 *     chassis_id  ↔  PlatformDescriptor::chassisId
 *     platform    ↔  PlatformDescriptor::platform
 *
 * --------------------------------------------------------------------------
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pwrseq::config
{

/** A single GPIO line reference (libgpiod line-name from DTS). */
struct LineRef
{
    std::string line;
    bool activeLow = false;

    bool empty() const noexcept
    {
        return line.empty();
    }
};

/** Optional lpu850 chardev path for boot-gated telemetry/log polling. */
struct TelemetryDescriptor
{
    std::string logDev; // e.g. "/dev/lpu15_log"

    bool empty() const noexcept
    {
        return logDev.empty();
    }
};

/** Optional four-bit firmware boot-stage bus. When present, the daemon
 *  publishes the encoded value and symbolic state name to the legacy /run
 *  files consumed by bmcweb. */
struct BootStatusDescriptor
{
    std::array<LineRef, 4> bits;
    bool enabled = false;
};

/** One element living inside a module. Elements within a module aggregate
 *  into the Module FSM. The kind is informational — the orchestrator picks
 *  the concrete driver class based on it (LPU → LpuDriver, …). */
struct ElementDescriptor
{
    enum class Kind : uint8_t
    {
        Lpu,
        Mcu, // future — placeholder for heterogeneous modules
        Gpu, // future
    };

    Kind kind = Kind::Lpu;
    std::string id; // human / Redfish ID, e.g. "HGX_LPU_3"

    LineRef pg;
    LineRef bc;
    LineRef resetLine; // LSTP reset output (assert only).
    TelemetryDescriptor telemetry;
    BootStatusDescriptor bootStatus;
};

/** A module groups one-or-more elements. The Module FSM aggregates child
 *  element states. */
struct ModuleDescriptor
{
    std::string id; // "HGX_LPU_Module_0" ...
    std::vector<ElementDescriptor> elements;
};

/** Per-FPGA descriptor. FPGAs run an Element FSM (same state machine as LPUs)
 *  and live directly under the Tray — no fault propagation to modules. */
struct FpgaDescriptor
{
    std::string id; // "HGX_FPGA_W" / "_E"

    LineRef pg;
    LineRef bc;
    LineRef resetLine;
    BootStatusDescriptor bootStatus;
};

/** Root document — the whole `platform.json` parses into one of these. */
struct PlatformDescriptor
{
    std::string platform; // "LP30-EB2"
    std::string chassisId{"HGX_Chassis_0"};
    std::chrono::milliseconds tBootMs{12'000};

    std::vector<ModuleDescriptor> modules;
    std::vector<FpgaDescriptor> fpgas;
};

// ──────────────────────────────────────────────────────────────────────────
// Loader API. See src/config/descriptor_loader.cpp for implementation.

/** Exception thrown by load() when the descriptor is malformed. The `what()`
 *  string identifies the offending JSON path and the reason, e.g.
 *      "modules[3].elements[1]: missing required field \"pg\""
 */
class DescriptorError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/** Parse a platform-descriptor JSON file. Throws DescriptorError on any
 *  schema violation; the returned descriptor is guaranteed structurally
 *  consistent (no duplicate IDs, every required LineRef populated, etc.).
 *
 *  Does NOT validate that the named libgpiod lines actually exist on the
 *  running kernel — that happens later in each driver's init().
 */
PlatformDescriptor loadFromFile(const std::string& path);

/** Same as loadFromFile but reads from an in-memory string. Convenience for
 *  unit tests. */
PlatformDescriptor loadFromString(const std::string& jsonText);

} // namespace pwrseq::config
