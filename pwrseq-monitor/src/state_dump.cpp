/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "fsm/state_dump.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <string>

namespace pwrseq
{

using nlohmann::json;

std::string utcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr)
    {
        return {};
    }
    char buf[32] = {};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
    {
        return {};
    }
    return buf;
}

std::string serializeFsmSnapshot(const FsmSnapshot& snap)
{
    json root;
    root["timestamp"] = snap.timestamp;
    root["platform"] = snap.platform;

    root["tray"] = json{
        {"id", snap.tray.id},
        {"state", std::string(to_string(snap.tray.state))},
    };

    json modules = json::array();
    for (const auto& m : snap.modules)
    {
        modules.push_back(json{
            {"id", m.id},
            {"state", std::string(to_string(m.state))},
            {"children", m.children},
        });
    }
    root["modules"] = std::move(modules);

    json elements = json::array();
    for (const auto& e : snap.elements)
    {
        elements.push_back(json{
            {"id", e.id},
            {"class", e.className},
            {"state", std::string(to_string(e.state))},
            {"fault", std::string(to_string(e.fault))},
            {"seen_pg_hi", e.seenPgHi},
            {"boot_timer_armed", e.bootTimerArmed},
            {"reset_pending", e.resetPending},
        });
    }
    root["elements"] = std::move(elements);

    json lines = json::array();
    for (const auto& l : snap.lines)
    {
        lines.push_back(json{
            {"name", l.name},
            {"has_value", l.hasValue},
            {"asserted", l.asserted},
            {"active_low", l.activeLow},
        });
    }
    root["lines"] = std::move(lines);

    return root.dump(2) + "\n";
}

bool writeFsmSnapshotFile(const std::string& path, const FsmSnapshot& snap)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << serializeFsmSnapshot(snap);
        out.flush();
        if (!out)
        {
            out.close();
            ::unlink(tmp.c_str());
            return false;
        }
    }

    if (::chmod(tmp.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
    {
        ::unlink(tmp.c_str());
        return false;
    }

    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace pwrseq
