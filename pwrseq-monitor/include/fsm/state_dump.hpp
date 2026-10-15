/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Point-in-time FSM + GPIO line-cache snapshot. Serialized to JSON for
 * Manager dumps (SIGUSR1 → /tmp/pwrseq_monitor_dump.json). No D-Bus
 * dependency; unit-tested in isolation.
 */

#pragma once

#include "core/types.hpp"

#include <string>
#include <vector>

namespace pwrseq
{

/** Well-known path written on SIGUSR1 and collected by dreport. */
inline constexpr const char* kFsmDumpPath = "/tmp/pwrseq_monitor_dump.json";

struct ElementSnapshot
{
    std::string  id;
    std::string  className;
    ElementState state = ElementState::Stby;
    FaultCode    fault = FaultCode::None;
    bool         seenPgHi = false;
    bool         bootTimerArmed = false;
    bool         resetPending = false;
};

struct ModuleSnapshot
{
    std::string              id;
    ModuleState              state = ModuleState::Stby;
    std::vector<std::string> children;
};

struct TraySnapshot
{
    std::string id;
    TrayState   state = TrayState::Stby;
};

struct LineSnapshot
{
    std::string name;
    bool        hasValue = false;
    bool        asserted = false;
    bool        activeLow = false;
};

struct FsmSnapshot
{
    std::string                  timestamp;
    std::string                  platform;
    TraySnapshot                 tray;
    std::vector<ModuleSnapshot>  modules;
    std::vector<ElementSnapshot> elements;
    std::vector<LineSnapshot>    lines;
};

/** UTC ISO-8601 timestamp (`YYYY-MM-DDTHH:MM:SSZ`). */
std::string utcTimestamp();

/** Serialize `snap` as pretty-printed JSON. */
std::string serializeFsmSnapshot(const FsmSnapshot& snap);

/** Write `snap` atomically (tmp + rename) to `path`. Returns false on I/O
 *  failure; the previous file (if any) is left untouched. */
bool writeFsmSnapshotFile(const std::string& path, const FsmSnapshot& snap);

}  // namespace pwrseq
