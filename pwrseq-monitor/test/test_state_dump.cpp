/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for FSM snapshot JSON serialization and atomic file write.
 */

#include "fsm/state_dump.hpp"

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

using nlohmann::json;
using namespace pwrseq;

namespace
{

FsmSnapshot sampleSnapshot()
{
    FsmSnapshot snap;
    snap.timestamp = "2026-10-07T18:16:00Z";
    snap.platform = "ts1-c00";
    snap.tray = {"HGX_Chassis_0", TrayState::Fault};
    snap.modules.push_back(ModuleSnapshot{
        "HGX_Module_0",
        ModuleState::Online,
        {"HGX_LPU_0", "HGX_LPU_1"},
    });
    snap.elements.push_back(ElementSnapshot{
        .id = "HGX_LPU_0",
        .className = "lpu",
        .state = ElementState::Online,
        .fault = FaultCode::None,
        .seenPgHi = true,
        .bootTimerArmed = false,
        .resetPending = false,
    });
    snap.elements.push_back(ElementSnapshot{
        .id = "HGX_LPU_1",
        .className = "lpu",
        .state = ElementState::Fault,
        .fault = FaultCode::BootTimeout,
        .seenPgHi = true,
        .bootTimerArmed = false,
        .resetPending = true,
    });
    snap.lines.push_back(LineSnapshot{
        .name = "MOD0_PGOOD_R-I",
        .hasValue = true,
        .asserted = true,
        .activeLow = false,
    });
    return snap;
}

std::filesystem::path tempDumpPath()
{
    auto path =
        std::filesystem::temp_directory_path() /
        ("pwrseq_state_dump_test." + std::to_string(::getpid()) + ".json");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
    return path;
}

} // namespace

TEST(StateDump, SerializesFsmAndLineCache)
{
    const json j = json::parse(serializeFsmSnapshot(sampleSnapshot()));

    EXPECT_EQ(j["timestamp"], "2026-10-07T18:16:00Z");
    EXPECT_EQ(j["platform"], "ts1-c00");
    EXPECT_EQ(j["tray"]["id"], "HGX_Chassis_0");
    EXPECT_EQ(j["tray"]["state"], "T_FAULT");

    ASSERT_EQ(j["modules"].size(), 1u);
    EXPECT_EQ(j["modules"][0]["id"], "HGX_Module_0");
    EXPECT_EQ(j["modules"][0]["state"], "M_ONLINE");
    EXPECT_EQ(j["modules"][0]["children"][1], "HGX_LPU_1");

    ASSERT_EQ(j["elements"].size(), 2u);
    EXPECT_EQ(j["elements"][0]["state"], "S_ONLINE");
    EXPECT_EQ(j["elements"][0]["fault"], "NONE");
    EXPECT_EQ(j["elements"][1]["state"], "S_FAULT");
    EXPECT_EQ(j["elements"][1]["fault"], "BOOT_TIMEOUT");
    EXPECT_TRUE(j["elements"][1]["reset_pending"].get<bool>());
    EXPECT_TRUE(j["elements"][0]["seen_pg_hi"].get<bool>());
    EXPECT_FALSE(j["elements"][0]["boot_timer_armed"].get<bool>());

    ASSERT_EQ(j["lines"].size(), 1u);
    EXPECT_EQ(j["lines"][0]["name"], "MOD0_PGOOD_R-I");
    EXPECT_TRUE(j["lines"][0]["has_value"].get<bool>());
    EXPECT_TRUE(j["lines"][0]["asserted"].get<bool>());
    EXPECT_FALSE(j["lines"][0]["active_low"].get<bool>());
}

TEST(StateDump, UtcTimestampLooksIso8601)
{
    const std::string ts = utcTimestamp();
    ASSERT_EQ(ts.size(), 20u);
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts.back(), 'Z');
}

TEST(StateDump, AtomicWriteRoundTrip)
{
    const auto path = tempDumpPath();
    ASSERT_TRUE(writeFsmSnapshotFile(path.string(), sampleSnapshot()));
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    std::ifstream in(path);
    json j;
    in >> j;
    EXPECT_EQ(j["elements"][1]["id"], "HGX_LPU_1");
    EXPECT_EQ(j["elements"][1]["fault"], "BOOT_TIMEOUT");

    std::filesystem::remove(path);
}

TEST(StateDump, FailedWriteLeavesPreviousFile)
{
    const auto path = tempDumpPath();
    ASSERT_TRUE(writeFsmSnapshotFile(path.string(), sampleSnapshot()));

    // A directory-as-tmp sibling cannot be opened as a file; rename must
    // not replace the existing snapshot.
    const auto tmpDir = std::filesystem::path(path.string() + ".tmp");
    std::filesystem::create_directory(tmpDir);
    FsmSnapshot empty;
    empty.platform = "should-not-land";
    EXPECT_FALSE(writeFsmSnapshotFile(path.string(), empty));

    std::ifstream in(path);
    json j;
    in >> j;
    EXPECT_EQ(j["platform"], "ts1-c00");

    std::filesystem::remove_all(tmpDir);
    std::filesystem::remove(path);
}
