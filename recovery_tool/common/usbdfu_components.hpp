// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

/**
 * Layout of the USB DFU recovery PLDM firmware package.
 *
 * The package carries the whole recovery kit, exactly like the USB RCM
 * recovery package carries its components: the preliminary boot bundle that
 * brings the AST2700 BootROM up to recovery U-Boot, followed by the SPI
 * firmware image.  PLDM extracts every component to
 *   <IMG_UPLOAD_DIR>/USBDFURecovery/<UUID>/<component id, decimal>/<file>
 * The item-updater watches those directories, the worker sends the bundle
 * stages in table order and then flashes the firmware image.
 *
 * The component identifiers below are the single source of truth for this
 * repository and must match the package definition owned by the release
 * team.
 *
 * The stage list is package-dependent, not platform-fixed: on P4102 the
 * Core-26.05-1_br release kit's MCU runtime requests zephyr-aspeed-ssp.bin and
 * zephyr-aspeed-tsp.bin after U-Boot (11 stages), while the develop
 * "verified" kit reaches recovery U-Boot after 9.  The Zephyr stages are
 * therefore optional components: sent when the package carries them, skipped
 * otherwise.
 */
namespace usbdfu
{

enum class ComponentRole : uint8_t
{
    /** Blob streamed to the BootROM / staged loader with plain dfu-util -D */
    BundleStage,
    /** Final SPI image written through recovery U-Boot's DFU alt setting */
    FirmwareImage,
};

struct Component
{
    uint16_t id;
    std::string_view name;
    ComponentRole role;
    /** May be absent from the package (package-dependent stage) */
    bool optional{false};
};

/** Order in this table is the DFU send order. */
inline constexpr std::array<Component, 12> componentMap{{
    {0x1, "Caliptra_FW", ComponentRole::BundleStage},
    {0x2, "SoC_Manifest", ComponentRole::BundleStage},
    {0x3, "MCU_Runtime", ComponentRole::BundleStage},
    {0x4, "DP_FW", ComponentRole::BundleStage},
    {0x5, "DDR_PMU_Train_IMEM", ComponentRole::BundleStage},
    {0x6, "DDR_PMU_Train_DMEM", ComponentRole::BundleStage},
    {0x7, "BL31", ComponentRole::BundleStage},
    {0x8, "TEE", ComponentRole::BundleStage},
    {0x9, "U-Boot", ComponentRole::BundleStage},
    {0xA, "Zephyr_SSP", ComponentRole::BundleStage, true},
    {0xB, "Zephyr_TSP", ComponentRole::BundleStage, true},
    {0x10, "HMC_SPI_Image", ComponentRole::FirmwareImage},
}};

} // namespace usbdfu
