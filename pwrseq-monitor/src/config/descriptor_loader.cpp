/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * JSON loader for the platform descriptor. nlohmann/json does the parsing;
 * this file does the schema validation and structural checks.
 *
 * Validation philosophy: fail FAST and LOUD with a JSON-path-style error
 * message ("modules[3].elements[1].pg: missing field \"line\""). The daemon
 * cannot recover from a bad descriptor — bring the service down rather than
 * silently mis-configure a tray.
 */

#include "config/descriptor.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <initializer_list>
#include <string_view>
#include <unordered_set>

namespace pwrseq::config
{

using nlohmann::json;

namespace
{

// ── Small helpers that throw DescriptorError with a JSON path prefix ───────

[[noreturn]] void fail(const std::string& path, const std::string& msg)
{
    throw DescriptorError(path + ": " + msg);
}

// path is taken by std::string_view (not const std::string&) so GCC 13's
// -Wdangling-reference heuristic does not flag callers that pass a string
// literal or a temporary concatenation -- with a const ref param, GCC pairs
// the returned `const json&` with the path temporary and (incorrectly)
// reports "possibly dangling reference to a temporary". The returned
// reference is actually a member of `obj`, never of `path`.
const json& requireField(const json& obj, const char* key,
                         std::string_view path)
{
    auto it = obj.find(key);
    if (it == obj.end())
    {
        fail(std::string(path),
             std::string("missing required field \"") + key + "\"");
    }
    return *it;
}

std::string requireString(const json& obj, const char* key,
                          const std::string& path)
{
    const auto& v = requireField(obj, key, path);
    if (!v.is_string())
    {
        fail(path + "." + key, "expected string");
    }
    return v.get<std::string>();
}

uint64_t optionalUint(const json& obj, const char* key, uint64_t dflt,
                      const std::string& path)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return dflt;
    if (!it->is_number_unsigned())
    {
        fail(path + "." + key, "expected non-negative integer");
    }
    return it->get<uint64_t>();
}

std::string optionalString(const json& obj, const char* key,
                           const std::string& dflt, const std::string& path)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return dflt;
    if (!it->is_string())
    {
        fail(path + "." + key, "expected string");
    }
    return it->get<std::string>();
}

bool optionalBool(const json& obj, const char* key, bool dflt,
                  const std::string& path)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return dflt;
    if (!it->is_boolean())
    {
        fail(path + "." + key, "expected boolean");
    }
    return it->get<bool>();
}

// Reject any keys in `obj` that aren't in `allowed`. Catches typos in
// optional keys (active_low / activeLow / acitve_low, tboot_ms /
// t_boot_ms, etc.) which would otherwise silently fall through to the
// optional* default values, and catches out-of-schema fields like the
// previously-tolerated fpgas[].kind. Caller passes the allowed key list
// inline at the parser site -- keeps the schema documented next to the
// parser logic for the same struct.
//
// Keys starting with '_' are treated as documentation/metadata and
// always allowed (standard JSON-without-comments convention, used in
// platform.json for e.g. "_comment_signal_provenance"). Typos in real
// optional keys never start with '_' so this still catches them.
void rejectUnknownKeys(const json& obj, const std::string& path,
                       std::initializer_list<const char*> allowed)
{
    if (!obj.is_object())
        return;
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        const std::string& k = it.key();
        if (!k.empty() && k[0] == '_')
            continue;
        bool ok = false;
        for (const char* a : allowed)
        {
            if (k == a)
            {
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            std::string joined;
            for (const char* a : allowed)
            {
                if (!joined.empty())
                    joined += ", ";
                joined += a;
            }
            fail(path + "." + k,
                 "unknown key (allowed: " + joined +
                     "). The descriptor "
                     "loader is strict: typos on optional keys would otherwise "
                     "silently fall through to defaults.");
        }
    }
}

// ── LineRef parser ─────────────────────────────────────────────────────────
//
// Schema: { "line": "LINE_NAME", "active_low": <bool, default false> }

LineRef parseLineRef(const json& obj, const std::string& path)
{
    if (!obj.is_object())
    {
        fail(path, "expected object with \"line\" field");
    }
    rejectUnknownKeys(obj, path, {"line", "active_low"});
    if (obj.contains("source"))
    {
        fail(path + ".source",
             "field \"source\" is no longer supported; PG/BC inputs are "
             "always monitored by phosphor-multi-gpio-monitor "
             "(Gpio.Monitor.Line PropertiesChanged → lpu-pwrseq-monitord)");
    }
    LineRef ref;
    ref.line = requireString(obj, "line", path);
    ref.activeLow = optionalBool(obj, "active_low", false, path);
    if (ref.line.empty())
    {
        fail(path + ".line", "must be non-empty");
    }
    return ref;
}

BootStatusDescriptor parseBootStatus(const json& obj, const std::string& path)
{
    if (!obj.is_array() || obj.size() != 4)
    {
        fail(path, "expected array of exactly four LineRef objects");
    }

    BootStatusDescriptor status;
    for (size_t i = 0; i < status.bits.size(); ++i)
    {
        status.bits[i] =
            parseLineRef(obj[i], path + "[" + std::to_string(i) + "]");
    }
    status.enabled = true;
    return status;
}

// ── Enum parser ────────────────────────────────────────────────────────────

ElementDescriptor::Kind parseKind(const std::string& s, const std::string& path)
{
    if (s == "LPU")
        return ElementDescriptor::Kind::Lpu;
    if (s == "MCU")
        return ElementDescriptor::Kind::Mcu;
    if (s == "GPU")
        return ElementDescriptor::Kind::Gpu;
    fail(path + ".kind", "unrecognised element kind \"" + s +
                             "\" "
                             "(expected one of: LPU, MCU, GPU)");
}

// ── Nested parsers ─────────────────────────────────────────────────────────

ElementDescriptor parseElement(const json& obj, const std::string& path)
{
    rejectUnknownKeys(obj, path,
                      {"kind", "id", "pg", "bc", "reset_line", "telemetry",
                       "boot_status_bits"});
    ElementDescriptor el;
    el.kind = parseKind(requireString(obj, "kind", path), path);
    el.id = requireString(obj, "id", path);
    if (el.id.empty())
    {
        fail(path + ".id", "must be non-empty");
    }
    el.pg = parseLineRef(requireField(obj, "pg", path), path + ".pg");
    el.bc = parseLineRef(requireField(obj, "bc", path), path + ".bc");
    el.resetLine = parseLineRef(requireField(obj, "reset_line", path),
                                path + ".reset_line");
    auto telIt = obj.find("telemetry");
    if (telIt != obj.end() && !telIt->is_null())
    {
        if (!telIt->is_object())
        {
            fail(path + ".telemetry", "expected object or null");
        }
        rejectUnknownKeys(*telIt, path + ".telemetry", {"log_dev"});
        el.telemetry.logDev =
            optionalString(*telIt, "log_dev", "", path + ".telemetry");
    }
    auto bootIt = obj.find("boot_status_bits");
    if (bootIt != obj.end() && !bootIt->is_null())
    {
        el.bootStatus = parseBootStatus(*bootIt, path + ".boot_status_bits");
    }
    return el;
}

ModuleDescriptor parseModule(const json& obj, const std::string& path)
{
    rejectUnknownKeys(obj, path, {"id", "elements"});
    ModuleDescriptor mod;
    mod.id = requireString(obj, "id", path);
    if (mod.id.empty())
    {
        fail(path + ".id", "must be non-empty");
    }

    const auto& elements = requireField(obj, "elements", path);
    if (!elements.is_array() || elements.empty())
    {
        fail(path + ".elements", "expected non-empty array");
    }
    mod.elements.reserve(elements.size());
    for (size_t i = 0; i < elements.size(); ++i)
    {
        mod.elements.push_back(parseElement(
            elements[i], path + ".elements[" + std::to_string(i) + "]"));
    }
    return mod;
}

FpgaDescriptor parseFpga(const json& obj, const std::string& path)
{
    rejectUnknownKeys(obj, path,
                      {"id", "pg", "bc", "reset_line", "boot_status_bits"});
    FpgaDescriptor fpga;
    fpga.id = requireString(obj, "id", path);
    if (fpga.id.empty())
    {
        fail(path + ".id", "must be non-empty");
    }
    fpga.pg = parseLineRef(requireField(obj, "pg", path), path + ".pg");
    fpga.bc = parseLineRef(requireField(obj, "bc", path), path + ".bc");
    fpga.resetLine = parseLineRef(requireField(obj, "reset_line", path),
                                  path + ".reset_line");
    auto bootIt = obj.find("boot_status_bits");
    if (bootIt != obj.end() && !bootIt->is_null())
    {
        fpga.bootStatus = parseBootStatus(*bootIt, path + ".boot_status_bits");
    }
    return fpga;
}

// ── Cross-field structural validation ──────────────────────────────────────
//
// JSON-level types are fine — but a descriptor still needs to be consistent:
//   * Module IDs unique
//   * Element IDs unique across the whole tray (Redfish needs uniqueness)
//   * FPGA IDs unique
//   * At least one module, at least one element per module

void validateConsistency(const PlatformDescriptor& d)
{
    if (d.modules.empty())
    {
        throw DescriptorError("modules: descriptor has no modules");
    }

    std::unordered_set<std::string> moduleIds;
    std::unordered_set<std::string> elementIds;

    for (size_t mi = 0; mi < d.modules.size(); ++mi)
    {
        const auto& m = d.modules[mi];
        const std::string mPath = "modules[" + std::to_string(mi) + "]";

        if (!moduleIds.insert(m.id).second)
        {
            throw DescriptorError(mPath + ".id: duplicate module ID \"" + m.id +
                                  "\"");
        }
        for (size_t ei = 0; ei < m.elements.size(); ++ei)
        {
            const auto& e = m.elements[ei];
            const std::string ePath =
                mPath + ".elements[" + std::to_string(ei) + "]";
            if (!elementIds.insert(e.id).second)
            {
                throw DescriptorError(ePath + ".id: duplicate element ID \"" +
                                      e.id + "\"");
            }
        }
    }

    std::unordered_set<std::string> fpgaIds;
    for (size_t fi = 0; fi < d.fpgas.size(); ++fi)
    {
        const auto& f = d.fpgas[fi];
        const std::string fPath = "fpgas[" + std::to_string(fi) + "]";
        if (!fpgaIds.insert(f.id).second)
        {
            throw DescriptorError(fPath + ".id: duplicate FPGA ID \"" + f.id +
                                  "\"");
        }
    }
}

PlatformDescriptor parseRoot(const json& root)
{
    if (!root.is_object())
    {
        fail("$", "top-level value must be a JSON object");
    }
    rejectUnknownKeys(
        root, "$", {"platform", "chassis_id", "t_boot_ms", "modules", "fpgas"});
    PlatformDescriptor d;
    d.platform = requireString(root, "platform", "$");
    d.chassisId = optionalString(root, "chassis_id", d.chassisId, "$");
    {
        const uint64_t raw = optionalUint(
            root, "t_boot_ms", static_cast<uint64_t>(d.tBootMs.count()), "$");
        // std::chrono::milliseconds is signed 64-bit. Reject values that
        // would wrap on construction, AND reject obviously-unreasonable
        // values (the daemon's deadline budget is per-element T_BOOT +
        // 3 s slack; any boot deadline beyond ~1 hour is misconfiguration).
        constexpr uint64_t kMaxBootMs = 60ULL * 60ULL * 1000ULL; // 1 h
        if (raw == 0)
        {
            fail("$.t_boot_ms", "must be > 0");
        }
        if (raw > kMaxBootMs)
        {
            fail("$.t_boot_ms",
                 "value " + std::to_string(raw) +
                     " ms exceeds the upper bound (3600000 ms = 1 h); "
                     "rejecting to avoid integer overflow / runaway "
                     "boot deadlines");
        }
        d.tBootMs = std::chrono::milliseconds(static_cast<int64_t>(raw));
    }

    const auto& modules = requireField(root, "modules", "$");
    if (!modules.is_array())
    {
        fail("$.modules", "expected array");
    }
    d.modules.reserve(modules.size());
    for (size_t i = 0; i < modules.size(); ++i)
    {
        d.modules.push_back(
            parseModule(modules[i], "$.modules[" + std::to_string(i) + "]"));
    }

    // FPGAs are optional — a board variant could have none.
    auto fIt = root.find("fpgas");
    if (fIt != root.end() && !fIt->is_null())
    {
        if (!fIt->is_array())
        {
            fail("$.fpgas", "expected array or null");
        }
        d.fpgas.reserve(fIt->size());
        for (size_t i = 0; i < fIt->size(); ++i)
        {
            d.fpgas.push_back(
                parseFpga((*fIt)[i], "$.fpgas[" + std::to_string(i) + "]"));
        }
    }

    validateConsistency(d);
    return d;
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────────

PlatformDescriptor loadFromFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        throw DescriptorError("cannot open descriptor file: " + path);
    }

    json root;
    try
    {
        in >> root;
    }
    catch (const json::parse_error& e)
    {
        throw DescriptorError(std::string("JSON parse error in ") + path +
                              ": " + e.what());
    }
    return parseRoot(root);
}

PlatformDescriptor loadFromString(const std::string& jsonText)
{
    json root;
    try
    {
        root = json::parse(jsonText);
    }
    catch (const json::parse_error& e)
    {
        throw DescriptorError(std::string("JSON parse error: ") + e.what());
    }
    return parseRoot(root);
}

} // namespace pwrseq::config
