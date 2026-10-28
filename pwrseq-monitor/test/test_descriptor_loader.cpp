/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the platform descriptor JSON loader.
 *
 * Hardware-free: every test uses an in-memory JSON string. The real LP30
 * platform.json lives in the meta-layer and is loaded at daemon startup.
 */

#include "config/descriptor.hpp"

#include <gtest/gtest.h>

using namespace pwrseq::config;

namespace
{

/** Minimal valid descriptor: 1 module / 1 LPU / 0 FPGAs.
 *  Useful baseline for negative tests that mutate one field. */
constexpr const char* kMinimal = R"JSON(
{
  "platform": "TEST",
  "modules": [
    {
      "id": "Mod_0",
      "elements": [
        {
          "kind": "LPU", "id": "LPU_0",
          "pg": { "line": "LPU1_PG" },
          "bc": { "line": "LPU1_BC" },
          "reset_line": { "line": "RST_O" }
        }
      ]
    }
  ]
}
)JSON";

} // namespace

// ── Happy path ────────────────────────────────────────────────────────────

TEST(DescriptorLoader, MinimalParsesOK)
{
    auto d = loadFromString(kMinimal);

    EXPECT_EQ(d.platform, "TEST");
    EXPECT_EQ(d.chassisId, "HGX_Chassis_0"); // default
    EXPECT_EQ(d.tBootMs.count(), 12000);     // default

    ASSERT_EQ(d.modules.size(), 1u);
    EXPECT_EQ(d.modules[0].id, "Mod_0");
    ASSERT_EQ(d.modules[0].elements.size(), 1u);

    const auto& el = d.modules[0].elements[0];
    EXPECT_EQ(el.kind, ElementDescriptor::Kind::Lpu);
    EXPECT_EQ(el.id, "LPU_0");
    EXPECT_EQ(el.pg.line, "LPU1_PG");
    EXPECT_FALSE(el.pg.activeLow);
    EXPECT_EQ(el.bc.line, "LPU1_BC");
    EXPECT_FALSE(el.bc.activeLow);
    EXPECT_EQ(el.resetLine.line, "RST_O");

    EXPECT_TRUE(d.fpgas.empty());
}

TEST(DescriptorLoader, FullStructureWithFpgas)
{
    constexpr const char* kFull = R"JSON(
    {
      "platform":   "LP30-EB2",
      "chassis_id": "HGX_Chassis_0",
      "t_boot_ms":  8000,
      "modules": [
        {
          "id": "M_0",
          "elements": [
            { "kind": "LPU", "id": "L_0",
              "pg": { "line": "p",  "active_low": false },
              "bc": { "line": "b" },
              "reset_line": { "line": "RST_O" } },
            { "kind": "LPU", "id": "L_1",
              "pg": { "line": "p2" },
              "bc": { "line": "b2" },
              "reset_line": { "line": "RST_O" } }
          ]
        }
      ],
      "fpgas": [
        {
          "id": "F_W",
          "pg":  { "line": "FPGA_PWR_GD" },
          "bc":  { "line": "INIT_DONE",   "active_low": true },
          "reset_line": { "line": "RST_O" }
        }
      ]
    }
    )JSON";

    auto d = loadFromString(kFull);
    EXPECT_EQ(d.platform, "LP30-EB2");
    EXPECT_EQ(d.chassisId, "HGX_Chassis_0");
    EXPECT_EQ(d.tBootMs.count(), 8000);

    ASSERT_EQ(d.fpgas.size(), 1u);
    EXPECT_EQ(d.fpgas[0].id, "F_W");
    EXPECT_EQ(d.fpgas[0].pg.line, "FPGA_PWR_GD");
    EXPECT_FALSE(d.fpgas[0].pg.activeLow);
    EXPECT_EQ(d.fpgas[0].bc.line, "INIT_DONE");
    EXPECT_TRUE(d.fpgas[0].bc.activeLow);
    EXPECT_EQ(d.fpgas[0].resetLine.line, "RST_O");
}

TEST(DescriptorLoader, ActiveLowDefaultsToFalseWhenOmitted)
{
    auto d = loadFromString(kMinimal);
    EXPECT_FALSE(d.modules[0].elements[0].pg.activeLow);
    EXPECT_FALSE(d.modules[0].elements[0].bc.activeLow);
}

TEST(DescriptorLoader, ActiveLowTrueParsedThrough)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "L", "active_low": true },
            "bc": { "line": "B", "active_low": true },
            "reset_line": { "line": "RST_O" } }
        ] } ] }
    )JSON";

    auto d = loadFromString(k);
    EXPECT_TRUE(d.modules[0].elements[0].pg.activeLow);
    EXPECT_TRUE(d.modules[0].elements[0].bc.activeLow);
}

TEST(DescriptorLoader, TelemetryLogDevOptional)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "HGX_LPU_14",
            "pg": { "line": "PG" },
            "bc": { "line": "BC" },
            "reset_line": { "line": "RST" },
            "telemetry": { "log_dev": "/dev/lpu15_log" } }
        ] } ] }
    )JSON";

    auto d = loadFromString(k);
    EXPECT_EQ(d.modules[0].elements[0].telemetry.logDev, "/dev/lpu15_log");

    auto d2 = loadFromString(kMinimal);
    EXPECT_TRUE(d2.modules[0].elements[0].telemetry.empty());
}

TEST(DescriptorLoader, TelemetryMustBeObjectWhenPresent)
{
    constexpr const char* kBadString = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "PG" },
            "bc": { "line": "BC" },
            "reset_line": { "line": "RST" },
            "telemetry": "bad" }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(kBadString), DescriptorError);

    constexpr const char* kBadArray = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "PG" },
            "bc": { "line": "BC" },
            "reset_line": { "line": "RST" },
            "telemetry": [] }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(kBadArray), DescriptorError);
}

TEST(DescriptorLoader, BootStatusBitsAreOptionalAndRequireFourLines)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "PG" },
            "bc": { "line": "BC" },
            "reset_line": { "line": "RST" },
            "boot_status_bits": [
              { "line": "STATE0" },
              { "line": "STATE1" },
              { "line": "STATE2" },
              { "line": "STATE3" }
            ] }
        ] } ],
      "fpgas": [
        { "id": "F0", "pg": { "line": "FPG" },
          "bc": { "line": "FBC" }, "reset_line": { "line": "FRST" },
          "boot_status_bits": [
            { "line": "FS0" }, { "line": "FS1" },
            { "line": "FS2" }, { "line": "FS3" }
          ] }
      ] }
    )JSON";

    auto d = loadFromString(k);
    ASSERT_TRUE(d.modules[0].elements[0].bootStatus.enabled);
    EXPECT_EQ(d.modules[0].elements[0].bootStatus.bits[3].line, "STATE3");
    ASSERT_TRUE(d.fpgas[0].bootStatus.enabled);
    EXPECT_EQ(d.fpgas[0].bootStatus.bits[0].line, "FS0");

    auto minimal = loadFromString(kMinimal);
    EXPECT_FALSE(minimal.modules[0].elements[0].bootStatus.enabled);

    constexpr const char* kBad = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "PG" }, "bc": { "line": "BC" },
            "reset_line": { "line": "RST" },
            "boot_status_bits": [
              { "line": "S0" }, { "line": "S1" }, { "line": "S2" }
            ] }
        ] } ] }
    )JSON";
    EXPECT_THROW(loadFromString(kBad), DescriptorError);
}

// ── Schema violations ─────────────────────────────────────────────────────

TEST(DescriptorLoader, MissingPlatformFieldThrows)
{
    constexpr const char* k = R"JSON(
    { "modules": [] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, EmptyModulesArrayThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X", "modules": [] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, ModuleWithNoElementsThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [ { "id": "M0", "elements": [] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, UnknownElementKindThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "FROBNOZ", "id": "X",
            "pg": { "line": "l" },
            "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, DuplicateModuleIdThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "l" }, "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] },
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L1",
            "pg": { "line": "l" }, "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] }
      ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, DuplicateElementIdAcrossModulesThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "Lx",
            "pg": { "line": "l" }, "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] },
        { "id": "M1", "elements": [
          { "kind": "LPU", "id": "Lx",
            "pg": { "line": "l" }, "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] }
      ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, DuplicateFpgaIdThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "l" }, "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] }
      ],
      "fpgas": [
        { "id": "F", "pg": { "line": "p" }, "bc": { "line": "i" },
          "reset_line": { "line": "RST_O" } },
        { "id": "F", "pg": { "line": "p" }, "bc": { "line": "i" },
          "reset_line": { "line": "RST_O" } }
      ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, MissingPgLineFieldThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "active_low": false },
            "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, EmptyPgLineThrows)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "" },
            "bc": { "line": "l" },
            "reset_line": { "line": "RST_O" } }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, SourceFieldRejected)
{
    constexpr const char* k = R"JSON(
    { "platform": "X",
      "modules": [
        { "id": "M0", "elements": [
          { "kind": "LPU", "id": "L0",
            "pg": { "line": "L", "source": "dbus" },
            "bc": { "line": "B" },
            "reset_line": { "line": "RST_O" } }
        ] } ] }
    )JSON";

    EXPECT_THROW(loadFromString(k), DescriptorError);
}

TEST(DescriptorLoader, MalformedJsonThrows)
{
    EXPECT_THROW(loadFromString("{ not valid json"), DescriptorError);
}

TEST(DescriptorLoader, NonexistentFileThrows)
{
    EXPECT_THROW(loadFromFile("/dev/null/does-not-exist"), DescriptorError);
}
