/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../recovery_tool/usbrcm_recovery_tool/ecid_parser.hpp"
#include "../recovery_tool/usbrcm_recovery_tool/progress_code_parser.hpp"

#include <array>
#include <cstring>
#include <utility>

#include "gtest/gtest.h"

// ========================== EcidParser Tests ==========================

using namespace EcidParser;

TEST(EcidParser, SwapNibbles)
{
    EXPECT_EQ(swapNibbles(0x10), 0x01);
    EXPECT_EQ(swapNibbles(0x30), 0x03);
    EXPECT_EQ(swapNibbles(0xA0), 0x0A);
    EXPECT_EQ(swapNibbles(0xAB), 0xBA);
    EXPECT_EQ(swapNibbles(0x00), 0x00);
    EXPECT_EQ(swapNibbles(0xFF), 0xFF);
    EXPECT_EQ(swapNibbles(0x12), 0x21);
}

TEST(EcidParser, IsOemKeyValidSet)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::BSI_OEM_KEY_VALID_BYTE] = BitField::BSI_OEM_KEY_VALID_MASK;
    EXPECT_TRUE(isOemKeyValid(ecid));
}

TEST(EcidParser, IsOemKeyValidNotSet)
{
    uint8_t ecid[ECID_SIZE] = {};
    EXPECT_FALSE(isOemKeyValid(ecid));
}

TEST(EcidParser, GetOwnershipStatusZero)
{
    uint8_t ecid[ECID_SIZE] = {};
    EXPECT_EQ(getOwnershipStatus(ecid), 0);
}

TEST(EcidParser, GetOwnershipStatusOdd)
{
    uint8_t ecid[ECID_SIZE] = {};
    // Set byte 20 to 0x10 (nibble-swapped: 0x01 -> count = 1)
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10;
    EXPECT_EQ(getOwnershipStatus(ecid), 1);
}

TEST(EcidParser, GetOwnershipStatusEven)
{
    uint8_t ecid[ECID_SIZE] = {};
    // 0x20 nibble-swapped: 0x02 -> count = 2
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x20;
    EXPECT_EQ(getOwnershipStatus(ecid), 2);
}

TEST(EcidParser, IsDOTBlobRequiredOddCount)
{
    uint8_t ecid[ECID_SIZE] = {};
    // OEM key not valid, ownership status odd (1)
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10;
    EXPECT_TRUE(isDOTBlobRequired(ecid));
}

TEST(EcidParser, IsDOTBlobNotRequiredEvenCount)
{
    uint8_t ecid[ECID_SIZE] = {};
    // OEM key not valid, ownership status even (2)
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x20;
    EXPECT_FALSE(isDOTBlobRequired(ecid));
}

TEST(EcidParser, IsDOTBlobNotRequiredOemKeyValid)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::BSI_OEM_KEY_VALID_BYTE] = BitField::BSI_OEM_KEY_VALID_MASK;
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10; // odd
    EXPECT_FALSE(isDOTBlobRequired(ecid));                // OEM key overrides
}

TEST(EcidParser, IsS2ABlobRequired)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::BSI_OEM_KEY_VALID_BYTE] = BitField::BSI_OEM_KEY_VALID_MASK;
    EXPECT_TRUE(isS2ABlobRequired(ecid));
}

TEST(EcidParser, IsS2ABlobNotRequired)
{
    uint8_t ecid[ECID_SIZE] = {};
    EXPECT_FALSE(isS2ABlobRequired(ecid));
}

TEST(EcidParser, GetBlobRequirementS2A)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::BSI_OEM_KEY_VALID_BYTE] = BitField::BSI_OEM_KEY_VALID_MASK;
    EXPECT_EQ(getBlobRequirement(ecid), BlobRequirement::S2ABlob);
}

TEST(EcidParser, GetBlobRequirementDOT)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10; // odd count
    EXPECT_EQ(getBlobRequirement(ecid), BlobRequirement::DOTBlob);
}

TEST(EcidParser, GetBlobRequirementNone)
{
    uint8_t ecid[ECID_SIZE] = {};
    EXPECT_EQ(getBlobRequirement(ecid), BlobRequirement::None);
}

TEST(EcidParser, BlobRequirementToStringS2A)
{
    auto s = blobRequirementToString(BlobRequirement::S2ABlob);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("S2A") != std::string_view::npos);
}

TEST(EcidParser, BlobRequirementToStringDOT)
{
    auto s = blobRequirementToString(BlobRequirement::DOTBlob);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("DOT") != std::string_view::npos);
}

TEST(EcidParser, BlobRequirementToStringNone)
{
    auto s = blobRequirementToString(BlobRequirement::None);
    EXPECT_TRUE(s.empty());
}

TEST(EcidParser, GetBlobRequirementString)
{
    uint8_t ecid[ECID_SIZE] = {};
    auto s = getBlobRequirementString(ecid);
    EXPECT_TRUE(s.empty()); // None
}

TEST(EcidParser, OwnershipStatusUsesHighByteAndFallbackStrings)
{
    uint8_t ecid[ECID_SIZE] = {};
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW] = 0x10;
    ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_HIGH] = 0x10;
    EXPECT_EQ(getOwnershipStatus(ecid), 0x101);
    EXPECT_EQ(getBlobRequirementString(ecid),
              "DOT blob required - Mutable DOT State");
    EXPECT_EQ(blobRequirementToString(static_cast<BlobRequirement>(0xFF)), "");
}

// ========================== ProgressCodeParser Tests
// ==========================

using namespace ProgressCodeParser;

TEST(ProgressCodeParser, DecodeBootSelVal)
{
    // bootSelVal = 0: Recovery mode, Invalid device, Enabled, STRAP,
    // ErrorInitiated
    auto info = decodeBootSelVal(0x00);
    EXPECT_EQ(info.bootMode, BootMode::Recovery);
    EXPECT_EQ(info.bootDevice, BootDevice::Invalid);
    EXPECT_EQ(info.deviceState, DeviceState::Enabled);
    EXPECT_EQ(info.selectionType, SelectionType::STRAP);
    EXPECT_EQ(info.recoveryType, RecoveryType::ErrorInitiated);
}

TEST(ProgressCodeParser, DecodeBootSelValColdboot)
{
    // bit 0 = 1 (ColdbootOrStreaming), bit 6 = 1 (Forced)
    auto info = decodeBootSelVal(0x41);
    EXPECT_EQ(info.bootMode, BootMode::ColdbootOrStreaming);
    EXPECT_EQ(info.recoveryType, RecoveryType::Forced);
}

TEST(ProgressCodeParser, DecodeBootSelValQSPIDevice)
{
    // bits 1-2 = 01 (QSPI)
    auto info = decodeBootSelVal(0x02);
    EXPECT_EQ(info.bootDevice, BootDevice::QSPI);
}

TEST(ProgressCodeParser, DecodeBootSelValOCPDevice)
{
    // bits 1-2 = 10 (OCP_RC)
    auto info = decodeBootSelVal(0x04);
    EXPECT_EQ(info.bootDevice, BootDevice::OCP_RC);
}

TEST(ProgressCodeParser, DecodeBootSelValUSBDevice)
{
    // bits 1-2 = 11 (USB)
    auto info = decodeBootSelVal(0x06);
    EXPECT_EQ(info.bootDevice, BootDevice::USB);
}

TEST(ProgressCodeParser, DecodeBootSelValDisabledDevice)
{
    // bit 3 = 1 (Disabled)
    auto info = decodeBootSelVal(0x08);
    EXPECT_EQ(info.deviceState, DeviceState::Disabled);
}

TEST(ProgressCodeParser, DecodeBootSelValOCPCMDSelection)
{
    // bits 4-5 = 01 (OCP_CMD)
    auto info = decodeBootSelVal(0x10);
    EXPECT_EQ(info.selectionType, SelectionType::OCP_CMD);
}

TEST(ProgressCodeParser, DecodeBootSelValFUSESelection)
{
    // bits 4-5 = 10 (FUSE)
    auto info = decodeBootSelVal(0x20);
    EXPECT_EQ(info.selectionType, SelectionType::FUSE);
}

TEST(ProgressCodeParser, DecodeBootSelValBCRSelection)
{
    // bits 4-5 = 11 (BCR)
    auto info = decodeBootSelVal(0x30);
    EXPECT_EQ(info.selectionType, SelectionType::BCR);
}

TEST(ProgressCodeParser, ExtractCpuIdPackage0)
{
    uint32_t pc = static_cast<uint32_t>(PackageClass::Package0) << 24;
    EXPECT_EQ(extractCpuId(pc), PackageClass::Package0);
}

TEST(ProgressCodeParser, ExtractCpuIdPackage1)
{
    uint32_t pc = static_cast<uint32_t>(PackageClass::Package1) << 24;
    EXPECT_EQ(extractCpuId(pc), PackageClass::Package1);
}

TEST(ProgressCodeParser, ExtractCpuIdUnknown)
{
    uint32_t pc = 0xFF000000;
    EXPECT_EQ(extractCpuId(pc), PackageClass::Unknown);
}

TEST(ProgressCodeParser, GetProgressCodeInfoKnown)
{
    // Test with a PSC_ROM progress code if we know one exists
    // The function looks up in static maps - just verify no crash
    auto info = getProgressCodeInfo(0);
    (void)info; // No crash
}

TEST(ProgressCodeParser, GetProgressCodeInfoUnknown)
{
    auto info = getProgressCodeInfo(0xFFFFFFFF);
    (void)info; // No crash, should return unknown info
}

TEST(ProgressCodeParser, BootModeToString)
{
    EXPECT_FALSE(bootModeToString(BootMode::Recovery).empty());
    EXPECT_FALSE(bootModeToString(BootMode::ColdbootOrStreaming).empty());
}

TEST(ProgressCodeParser, BootDeviceToString)
{
    EXPECT_FALSE(bootDeviceToString(BootDevice::QSPI).empty());
    EXPECT_FALSE(bootDeviceToString(BootDevice::OCP_RC).empty());
    EXPECT_FALSE(bootDeviceToString(BootDevice::USB).empty());
    EXPECT_FALSE(bootDeviceToString(BootDevice::Invalid).empty());
}

TEST(ProgressCodeParser, SelectionTypeToString)
{
    EXPECT_FALSE(selectionTypeToString(SelectionType::STRAP).empty());
    EXPECT_FALSE(selectionTypeToString(SelectionType::OCP_CMD).empty());
    EXPECT_FALSE(selectionTypeToString(SelectionType::FUSE).empty());
    EXPECT_FALSE(selectionTypeToString(SelectionType::BCR).empty());
}

TEST(ProgressCodeParser, RecoveryTypeToString)
{
    EXPECT_FALSE(recoveryTypeToString(RecoveryType::ErrorInitiated).empty());
    EXPECT_FALSE(recoveryTypeToString(RecoveryType::Forced).empty());
}

TEST(ProgressCodeParser, CodeTypeAndPackageStringsCoverAllKnownValues)
{
    EXPECT_EQ(codeTypeToString(CodeType::Progress), "Progress");
    EXPECT_EQ(codeTypeToString(CodeType::Error), "Error");
    EXPECT_EQ(codeTypeToString(CodeType::Debug), "Debug");

    EXPECT_EQ(packageClassToString(PackageClass::Package0), "CPU0");
    EXPECT_EQ(packageClassToString(PackageClass::Package1), "CPU1");
    EXPECT_EQ(packageClassToString(PackageClass::Unknown), "Unknown");
    EXPECT_EQ(packageClassToCpuId(PackageClass::Package0), 0);
    EXPECT_EQ(packageClassToCpuId(PackageClass::Package1), 1);
    EXPECT_EQ(packageClassToCpuId(PackageClass::Unknown), 255);
}

TEST(ProgressCodeParser, EnumStringFallbacksCoverInvalidValues)
{
    EXPECT_EQ(codeTypeToString(static_cast<CodeType>(0xFF)), "Unknown");
    EXPECT_EQ(packageClassToString(static_cast<PackageClass>(0x00)), "Unknown");
    EXPECT_EQ(subClassToString(static_cast<SubClass>(0x00)), "Unknown");
    EXPECT_EQ(bootModeToString(static_cast<BootMode>(0xFF)), "Unknown");
    EXPECT_EQ(bootDeviceToString(static_cast<BootDevice>(0xFF)), "Unknown");
    EXPECT_EQ(selectionTypeToString(static_cast<SelectionType>(0xFF)),
              "Unknown");
    EXPECT_EQ(recoveryTypeToString(static_cast<RecoveryType>(0xFF)), "Unknown");
    EXPECT_EQ(packageClassToCpuId(static_cast<PackageClass>(0x00)), 255);
}

TEST(ProgressCodeParser, SubClassToStringCoversRepresentativeMappings)
{
    EXPECT_EQ(subClassToString(SubClass::PSC_ROM), "PSC_ROM");
    EXPECT_EQ(subClassToString(SubClass::PSC_FMC), "PSC_FMC");
    EXPECT_EQ(subClassToString(SubClass::PSC_RT), "PSC_RT");
    EXPECT_EQ(subClassToString(SubClass::MB1), "MB1");
    EXPECT_EQ(subClassToString(SubClass::UEFI), "UEFI");
    EXPECT_EQ(subClassToString(SubClass::OOBHUB_FW), "OOBHUB_FW");
    EXPECT_EQ(subClassToString(SubClass::PCORE5_FW), "PCORE5_FW");
    EXPECT_EQ(subClassToString(SubClass::C2C_LPI_C_1), "C2C_LPI_C_1");
}

TEST(ProgressCodeParser, SubClassToStringCoversRemainingMappings)
{
    const std::array<std::pair<SubClass, std::string_view>, 24> expectations{{
        {SubClass::BPMP_FW, "BPMP_FW"},
        {SubClass::MB2, "MB2"},
        {SubClass::ATF_BL31, "ATF_BL31"},
        {SubClass::RMM, "RMM"},
        {SubClass::Hafnium, "Hafnium"},
        {SubClass::UEFI_StMM, "UEFI_StMM"},
        {SubClass::RAS_FW, "RAS_FW"},
        {SubClass::MSEQ_FW, "MSEQ_FW"},
        {SubClass::PCORE0_FW, "PCORE0_FW"},
        {SubClass::PCORE1_FW, "PCORE1_FW"},
        {SubClass::PCORE2_FW, "PCORE2_FW"},
        {SubClass::PCORE3_FW, "PCORE3_FW"},
        {SubClass::PCORE4_FW, "PCORE4_FW"},
        {SubClass::C2C_GRS_0, "C2C_GRS_0"},
        {SubClass::C2C_GRS_1, "C2C_GRS_1"},
        {SubClass::C2C_UPHY_0, "C2C_UPHY_0"},
        {SubClass::C2C_UPHY_1, "C2C_UPHY_1"},
        {SubClass::C2C_UPHY_2, "C2C_UPHY_2"},
        {SubClass::C2C_UPHY_3, "C2C_UPHY_3"},
        {SubClass::C2C_UPHY_4, "C2C_UPHY_4"},
        {SubClass::C2C_UPHY_5, "C2C_UPHY_5"},
        {SubClass::C2C_LPI_S_0, "C2C_LPI_S_0"},
        {SubClass::C2C_LPI_S_1, "C2C_LPI_S_1"},
        {SubClass::C2C_LPI_C_0, "C2C_LPI_C_0"},
    }};

    for (const auto& [value, expected] : expectations)
    {
        EXPECT_EQ(subClassToString(value), expected);
    }
}

TEST(ProgressCodeParser, BitfieldExtractorsDecodeProgressCode)
{
    constexpr uint32_t progressCode = 0x70C0C042;
    EXPECT_EQ(getCodeType(progressCode), CodeType::Progress);
    EXPECT_EQ(getSubClass(progressCode), SubClass::PSC_ROM);
    EXPECT_EQ(getOperation(progressCode), 0x02);
    EXPECT_EQ(getBootSelVal(progressCode), 1);
}

TEST(ProgressCodeParser, RecoveryCompleteRecognizesBothCpuCodes)
{
    EXPECT_TRUE(isRecoveryComplete(RecoveryCompleteCodes::CPU0));
    EXPECT_TRUE(isRecoveryComplete(RecoveryCompleteCodes::CPU1));
    EXPECT_FALSE(isRecoveryComplete(0x70C10007));
}

TEST(ProgressCodeParser, MapOperationToStringCoversAllMappedFamilies)
{
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_ROM, 0x02),
              "PSC_ROM_PC_BOOT_MODE_SEL_DONE");
    EXPECT_EQ(mapOperationToString(CodeType::Error, SubClass::PSC_ROM, 0x02),
              "PSC_ROM_EC_BOOT_MODE_SEL_FAIL");
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_FMC, 0x07),
              "PSC_FMC_PC_DEBUG_TOKEN_LOAD");
    EXPECT_EQ(mapOperationToString(CodeType::Error, SubClass::PSC_FMC, 0x06),
              "PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL");
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_RT, 0x02),
              "PSC_RT_PC_PLDM_T5_READY");
    EXPECT_EQ(
        mapOperationToString(CodeType::Progress, SubClass::OOBHUB_FW, 0x01),
        "OOBHUB_PC_INIT");
    EXPECT_EQ(mapOperationToString(CodeType::Debug, SubClass::PSC_ROM, 0x02),
              "");
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::MB1, 0x01),
              "");
}

TEST(ProgressCodeParser, MapOperationToStringCoversUnmappedAndBoundaryCases)
{
    EXPECT_EQ(mapOperationToString(CodeType::Error, SubClass::PSC_ROM, 0x00),
              "");
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_ROM, 0x00),
              "");
    EXPECT_EQ(
        mapOperationToString(CodeType::Error, SubClass::PSC_ROM,
                             static_cast<uint8_t>(Detail::PSC_ROM_ERROR_COUNT)),
        "");
    EXPECT_EQ(mapOperationToString(
                  CodeType::Progress, SubClass::PSC_ROM,
                  static_cast<uint8_t>(Detail::PSC_ROM_PROGRESS_COUNT)),
              "");

    EXPECT_EQ(mapOperationToString(CodeType::Error, SubClass::PSC_FMC, 0x00),
              "");
    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_FMC, 0x00),
              "");
    EXPECT_EQ(
        mapOperationToString(CodeType::Error, SubClass::PSC_FMC,
                             static_cast<uint8_t>(Detail::PSC_FMC_ERROR_COUNT)),
        "");
    EXPECT_EQ(mapOperationToString(
                  CodeType::Progress, SubClass::PSC_FMC,
                  static_cast<uint8_t>(Detail::PSC_FMC_PROGRESS_COUNT)),
              "");

    EXPECT_EQ(mapOperationToString(CodeType::Progress, SubClass::PSC_RT, 0x01),
              "");
    EXPECT_EQ(mapOperationToString(
                  CodeType::Progress, SubClass::PSC_RT,
                  static_cast<uint8_t>(Detail::PSC_RT_PROGRESS_COUNT)),
              "");

    EXPECT_EQ(
        mapOperationToString(CodeType::Progress, SubClass::OOBHUB_FW, 0x00),
        "");
    EXPECT_EQ(
        mapOperationToString(CodeType::Progress, SubClass::OOBHUB_FW, 0x02),
        "");
    EXPECT_EQ(mapOperationToString(
                  CodeType::Progress, SubClass::OOBHUB_FW,
                  static_cast<uint8_t>(Detail::OOBHUB_PROGRESS_COUNT)),
              "");

    EXPECT_EQ(mapOperationToString(CodeType::Error, SubClass::OOBHUB_FW, 0x01),
              "");
    EXPECT_EQ(mapOperationToString(CodeType::Debug, SubClass::PSC_FMC, 0x01),
              "");
}

TEST(ProgressCodeParser, GetProgressCodeInfoPopulatesBootSelectionInfo)
{
    constexpr uint32_t recoveryBootCode = 0x70C0C002;
    auto info = getProgressCodeInfo(recoveryBootCode);
    EXPECT_EQ(info.type, CodeType::Progress);
    EXPECT_EQ(info.subClass, SubClass::PSC_ROM);
    EXPECT_EQ(info.operation, 0x02);
    EXPECT_EQ(info.cpuId, PackageClass::Package0);
    EXPECT_EQ(info.name, "PSC_ROM_PC_BOOT_MODE_SEL_DONE");
    ASSERT_TRUE(info.bootSelectionInfo.has_value());
    EXPECT_EQ(info.bootSelectionInfo->bootMode, BootMode::Recovery);
    EXPECT_FALSE(info.getBootSelectionString().empty());
}

TEST(ProgressCodeParser, GetProgressCodeInfoCoversColdbootOobhubAndFallbacks)
{
    constexpr uint32_t coldbootCode = 0x70C0D042;
    auto coldboot = getProgressCodeInfo(coldbootCode);
    ASSERT_TRUE(coldboot.bootSelectionInfo.has_value());
    EXPECT_EQ(coldboot.bootSelectionInfo->bootMode,
              BootMode::ColdbootOrStreaming);
    EXPECT_NE(coldboot.getBootSelectionString().find("Coldboot/Streaming"),
              std::string::npos);
    EXPECT_NE(coldboot.getBootSelectionString().find("Recovery type: "),
              std::string::npos);

    constexpr uint32_t cpu1OobhubCode =
        (static_cast<uint32_t>(CodeType::Progress) << 30) |
        (static_cast<uint32_t>(PackageClass::Package1) << 24) |
        (static_cast<uint32_t>(SubClass::OOBHUB_FW) << 16) | 0x03;
    auto oobhub = getProgressCodeInfo(cpu1OobhubCode);
    EXPECT_EQ(oobhub.cpuId, PackageClass::Package1);
    EXPECT_EQ(oobhub.name, "OOBHUB_PC_MCTP_INIT");
    EXPECT_FALSE(oobhub.bootSelectionInfo.has_value());

    constexpr uint32_t unmappedRtCode =
        (static_cast<uint32_t>(CodeType::Progress) << 30) |
        (static_cast<uint32_t>(PackageClass::Package0) << 24) |
        (static_cast<uint32_t>(SubClass::PSC_RT) << 16) | 0x01;
    auto unmapped = getProgressCodeInfo(unmappedRtCode);
    EXPECT_TRUE(unmapped.name.empty());
    EXPECT_FALSE(unmapped.bootSelectionInfo.has_value());
}

TEST(ProgressCodeParser, GetProgressCodeInfoHandlesOtherMappedAndUnknownCodes)
{
    auto progress = getProgressCodeInfo(0x70C10007);
    EXPECT_EQ(progress.name, "PSC_FMC_PC_DEBUG_TOKEN_LOAD");
    EXPECT_FALSE(progress.bootSelectionInfo.has_value());

    auto error = getProgressCodeInfo(0xB0C10006);
    EXPECT_EQ(error.type, CodeType::Error);
    EXPECT_EQ(error.name, "PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL");

    auto unknown = getProgressCodeInfo(0x30DD0001);
    EXPECT_TRUE(unknown.name.empty());
    EXPECT_EQ(unknown.cpuId, PackageClass::Package0);
}
