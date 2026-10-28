// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "mcu_recovery_model.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace nvidia::software::updater::mcu
{
namespace
{

Env env(const std::string& text)
{
    std::istringstream in(text);
    return parseEnv(in);
}

// The vr-nvl-bmc file as it shipped before it was removed from the layer.
const std::string vrNvlBmc = R"(INSTANCE_COUNT=2
TARGET_NAME_0=CX9
TARGET_NAME_1=BF4

CX9_MODEL=Nvidia:CX9:8F6B4D3E2C2B4B39BC929C6C4DF7B8E1

BF4_VARIANT_DETECT=FRU
BF4_FRU_BUS=72
BF4_FRU_ADDR=80
BF4_VARIANT_COUNT=2
# B4240
BF4_VARIANT_0_KEY=D4A4
BF4_VARIANT_0_MODEL=Nvidia:BF4:C8E5F4F05A2E4C6B91512E4D7C3CE4C6
# C9180
BF4_VARIANT_1_KEY=X9ME
BF4_VARIANT_1_MODEL=Nvidia:BF4:668BD433B9F8473DBA1EC32B99772679
)";

struct Fru
{
    std::vector<std::pair<std::string, std::string>> asked;
    std::vector<std::optional<std::string>> answers;
    size_t next = 0;
    FruLookup fn()
    {
        return [this](const std::string& b, const std::string& a) {
            asked.emplace_back(b, a);
            return next < answers.size() ? answers[next++] : std::nullopt;
        };
    }
};

struct Clock
{
    int sleeps = 0;
    Sleep fn()
    {
        return [this](std::chrono::seconds s) {
            EXPECT_EQ(s, std::chrono::seconds(5));
            ++sleeps;
        };
    }
};

TEST(McuEnv, ParsesShellStyleFile)
{
    auto e = env("# comment\n\nA=1\n B = two \nC=\"quoted value\"\nD='x'\n"
                 "noequals\nE=\n");
    EXPECT_EQ(e.at("A"), "1");
    EXPECT_EQ(e.at("B"), "two");
    EXPECT_EQ(e.at("C"), "quoted value");
    EXPECT_EQ(e.at("D"), "x");
    EXPECT_EQ(e.at("E"), "");
    EXPECT_FALSE(e.contains("noequals"));
}

TEST(McuModel, FixedModel)
{
    Fru fru;
    Clock clock;
    std::ostringstream err;
    EXPECT_EQ(resolveModel(env(vrNvlBmc), "CX9", fru.fn(), clock.fn(), err),
              "Nvidia:CX9:8F6B4D3E2C2B4B39BC929C6C4DF7B8E1");
    EXPECT_TRUE(fru.asked.empty());
    EXPECT_TRUE(err.str().empty());
}

TEST(McuModel, MissingFixedModel)
{
    Fru fru;
    Clock clock;
    std::ostringstream err;
    EXPECT_FALSE(resolveModel(env(vrNvlBmc), "HPM", fru.fn(), clock.fn(), err));
    EXPECT_EQ(err.str(), "Error: HPM_MODEL not set in environment file\n");
}

TEST(McuModel, EmptyInstance)
{
    Fru fru;
    Clock clock;
    std::ostringstream err;
    EXPECT_FALSE(resolveModel(env(vrNvlBmc), "", fru.fn(), clock.fn(), err));
    EXPECT_EQ(err.str(), "Error: instance name required\n");
}

TEST(McuModel, FruVariantFirstMatchingKeyWins)
{
    Fru fru;
    fru.answers = {std::string("900-9D3D4-X9ME-ZZ0")};
    Clock clock;
    std::ostringstream err;
    EXPECT_EQ(resolveModel(env(vrNvlBmc), "BF4", fru.fn(), clock.fn(), err),
              "Nvidia:BF4:668BD433B9F8473DBA1EC32B99772679");
    ASSERT_EQ(fru.asked.size(), 1U);
    EXPECT_EQ(fru.asked[0],
              std::make_pair(std::string("72"), std::string("80")));
    EXPECT_EQ(clock.sleeps, 0);
}

TEST(McuModel, FruRetriesUntilFound)
{
    Fru fru;
    fru.answers = {std::nullopt, std::string(""),
                   std::string("900-9D3D4-D4A4-ZZ0")};
    Clock clock;
    std::ostringstream err;
    EXPECT_EQ(resolveModel(env(vrNvlBmc), "BF4", fru.fn(), clock.fn(), err),
              "Nvidia:BF4:C8E5F4F05A2E4C6B91512E4D7C3CE4C6");
    EXPECT_EQ(clock.sleeps, 2);
    EXPECT_EQ(err.str(),
              "FRU not found at BUS=72 ADDR=80, retrying in 5s (1/60)...\n"
              "FRU not found at BUS=72 ADDR=80, retrying in 5s (2/60)...\n");
}

TEST(McuModel, FruGivesUpAfterSixtyTries)
{
    Fru fru;
    Clock clock;
    std::ostringstream err;
    EXPECT_FALSE(resolveModel(env(vrNvlBmc), "BF4", fru.fn(), clock.fn(), err));
    EXPECT_EQ(fru.asked.size(), 60U);
    EXPECT_EQ(clock.sleeps, 60);
    EXPECT_NE(
        err.str().find("Error: No FRU found at BUS=72 ADDR=80 after 300s\n"),
        std::string::npos);
}

TEST(McuModel, FruUnknownVariant)
{
    Fru fru;
    fru.answers = {std::string("699-123")};
    Clock clock;
    std::ostringstream err;
    EXPECT_FALSE(resolveModel(env(vrNvlBmc), "BF4", fru.fn(), clock.fn(), err));
    EXPECT_EQ(err.str(),
              "Error: Unknown BF4 variant, BOARD_PART_NUMBER=699-123\n");
}

TEST(McuModel, FruMissingBusOrCount)
{
    Fru fru;
    Clock clock;
    std::ostringstream err;
    auto e = env("X_VARIANT_DETECT=FRU\nX_FRU_BUS=1\n");
    EXPECT_FALSE(resolveModel(e, "X", fru.fn(), clock.fn(), err));
    EXPECT_EQ(err.str(), "Error: X_FRU_BUS or X_FRU_ADDR not set\n");

    std::ostringstream err2;
    fru.answers = {std::string("P")};
    auto e2 = env("X_VARIANT_DETECT=FRU\nX_FRU_BUS=1\nX_FRU_ADDR=2\n");
    EXPECT_FALSE(resolveModel(e2, "X", fru.fn(), clock.fn(), err2));
    EXPECT_EQ(err2.str(), "Error: X_VARIANT_COUNT not set\n");

    std::ostringstream err3;
    fru.answers.push_back(std::string("P"));
    EXPECT_FALSE(resolveModel(env("X_VARIANT_DETECT=FRU\nX_FRU_BUS=1\n"
                                  "X_FRU_ADDR=2\nX_VARIANT_COUNT=two\n"),
                              "X", fru.fn(), clock.fn(), err3));
    EXPECT_EQ(err3.str(), "Error: X_VARIANT_COUNT is not a number\n");
}

TEST(McuModel, FruVariantRejectsEmptyKeyOrModel)
{
    Fru fru;
    fru.answers = {std::string("900-9D3D4-X9ME-ZZ0"),
                   std::string("900-9D3D4-X9ME-ZZ0")};
    Clock clock;
    std::ostringstream err;
    auto base = std::string("X_VARIANT_DETECT=FRU\nX_FRU_BUS=1\nX_FRU_ADDR=2\n"
                            "X_VARIANT_COUNT=2\n"
                            "X_VARIANT_1_KEY=X9ME\nX_VARIANT_1_MODEL=M1\n");
    EXPECT_FALSE(resolveModel(env(base + "X_VARIANT_0_MODEL=M0\n"), "X",
                              fru.fn(), clock.fn(), err));
    EXPECT_EQ(err.str(),
              "Error: X_VARIANT_0_KEY or X_VARIANT_0_MODEL not set\n");

    std::ostringstream err2;
    EXPECT_FALSE(resolveModel(env(base + "X_VARIANT_0_KEY=D4A4\n"), "X",
                              fru.fn(), clock.fn(), err2));
    EXPECT_EQ(err2.str(),
              "Error: X_VARIANT_0_KEY or X_VARIANT_0_MODEL not set\n");
}

TEST(McuStarter, StartsEveryInstanceInOrder)
{
    std::vector<std::string> started;
    std::ostringstream out;
    EXPECT_EQ(startUpdaters(
                  env(vrNvlBmc),
                  [&](const std::string& u) { started.push_back(u); }, out),
              0);
    EXPECT_EQ(started, (std::vector<std::string>{
                           "com.Nvidia.MCURecovery.Updater@CX9.service",
                           "com.Nvidia.MCURecovery.Updater@BF4.service"}));
    EXPECT_EQ(out.str(),
              "Starting com.Nvidia.MCURecovery.Updater@CX9.service\n"
              "Starting com.Nvidia.MCURecovery.Updater@BF4.service\n");
}

TEST(McuStarter, Errors)
{
    auto none = [](const std::string&) {};
    std::ostringstream a, b, c;
    EXPECT_EQ(startUpdaters(env(""), none, a), 1);
    EXPECT_EQ(a.str(), "Error: INSTANCE_COUNT not set\n");
    EXPECT_EQ(
        startUpdaters(env("INSTANCE_COUNT=2\nTARGET_NAME_0=A\n"), none, b), 1);
    EXPECT_NE(b.str().find("Error: TARGET_NAME_1 not set\n"),
              std::string::npos);
    auto failing = [](const std::string&) {
        throw std::runtime_error("Unit not found");
    };
    EXPECT_EQ(
        startUpdaters(env("INSTANCE_COUNT=1\nTARGET_NAME_0=A\n"), failing, c),
        1);
    EXPECT_EQ(startUpdaters(env("INSTANCE_COUNT=0\n"), none, c), 0);
    std::ostringstream d;
    EXPECT_EQ(startUpdaters(env("INSTANCE_COUNT=x\n"), none, d), 1);
    EXPECT_EQ(d.str(), "Error: INSTANCE_COUNT is not a number\n");
    std::ostringstream n;
    std::vector<std::string> started;
    EXPECT_EQ(startUpdaters(
                  env("INSTANCE_COUNT=-1\nTARGET_NAME_0=A\n"),
                  [&](const std::string& u) { started.push_back(u); }, n),
              1);
    EXPECT_EQ(n.str(), "Error: INSTANCE_COUNT is negative\n");
    EXPECT_TRUE(started.empty());
}

} // namespace
} // namespace nvidia::software::updater::mcu
