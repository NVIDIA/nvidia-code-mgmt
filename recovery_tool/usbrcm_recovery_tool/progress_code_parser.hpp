/**
 * @file progress_code_parser.hpp
 * @brief Header-only parser for CPU 32-bit boot progress codes
 *
 * API usage:
 *   auto info = getProgressCodeInfo(progressCode);
 *   info.name              // Human-readable string (hex format if unmapped)
 *   info.type              // CodeType: Progress/Error/Debug
 *   info.cpuId             // PackageClass: Package0/Package1/Unknown
 *   info.subClass          // SubClass: PSC_ROM, PSC_FMC, etc.
 *   info.operation         // Raw operation code (bits 5:0)
 *   info.bootSelectionInfo // Optional boot selection info (for
 * BOOT_MODE_SEL_DONE) info.getBootSelectionString()  // Boot selection details
 * (empty if not available)
 *
 * Example:
 *   auto info = ProgressCodeParser::getProgressCodeInfo(0x70c0c002);
 *   if (info.bootSelectionInfo.has_value()) {
 *       std::cout << "Boot mode: " << info.getBootSelectionString();
 *   }
 *   std::cout << "Code: " << info.name;  // "PSC_ROM_PC_BOOT_MODE_SEL_DONE" or
 * "0xXXXXXXXX"
 */

#pragma once

#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace ProgressCodeParser
{

// Progress Code Types (bits 31:30)
enum class CodeType : uint8_t
{
    Progress = 0x01,
    Error = 0x02,
    Debug = 0x03
};

// CPU Package Classes (bits 29:24) - CPU identification
enum class PackageClass : uint8_t
{
    Package0 = 0x30, // CPU 0
    Package1 = 0x31, // CPU 1
    Unknown = 0xFF   // Unknown/invalid package
};

// Status Code Sub Classes (bits 23:16)
enum class SubClass : uint8_t
{
    PSC_ROM = 0xC0,
    PSC_FMC = 0xC1,
    PSC_RT = 0xC2,
    MB1 = 0xC3,
    BPMP_FW = 0xC4,
    MB2 = 0xC5,
    ATF_BL31 = 0xC6,
    RMM = 0xC7,
    Hafnium = 0xC8,
    UEFI = 0xC9,
    UEFI_StMM = 0xCA,
    OOBHUB_FW = 0xCB,
    RAS_FW = 0xCC,
    MSEQ_FW = 0xCD,
    PCORE0_FW = 0xCE,
    PCORE1_FW = 0xCF,
    PCORE2_FW = 0xD0,
    PCORE3_FW = 0xD1,
    PCORE4_FW = 0xD2,
    PCORE5_FW = 0xD3,
    C2C_GRS_0 = 0xD4,
    C2C_GRS_1 = 0xD5,
    C2C_UPHY_0 = 0xD6,
    C2C_UPHY_1 = 0xD7,
    C2C_UPHY_2 = 0xD8,
    C2C_UPHY_3 = 0xD9,
    C2C_UPHY_4 = 0xDA,
    C2C_UPHY_5 = 0xDB,
    C2C_LPI_S_0 = 0xDC,
    C2C_LPI_S_1 = 0xDD,
    C2C_LPI_C_0 = 0xDE,
    C2C_LPI_C_1 = 0xDF
};

// PSC ROM Progress Codes (Status Code operation field, Bits 5:0)
enum class PscRomProgressCode : uint8_t
{
    I2C_EXT_MSG_INIT = 0x01,
    BOOT_MODE_SEL_DONE = 0x02,
    USB_EXT_MSG_INIT = 0x03,
    QSPI0_DEV_INIT = 0x04,
    USB2_DEV_INIT = 0x05,
    OCPRC_DEV_INIT = 0x06,
    BOOT_CHAIN_SEL = 0x07,
    BOOT_IMAGE_LOAD_DONE = 0x08,
    DOT_S2A_VALIDATION_DONE = 0x09,
    FMC_VALIDATION_DONE = 0x0A,
    ROM_EXIT = 0x0B,
    DOT_RECOVERY = 0x0C
};

// PSC ROM Error Codes (Status Code operation field, Bits 5:0)
enum class PscRomErrorCode : uint8_t
{
    I2C_EXT_MSG_FAIL = 0x01,
    BOOT_MODE_SEL_FAIL = 0x02,
    USB_EXT_MSG_FAIL = 0x03,
    EROT_GRANT_FAIL = 0x04,
    QSPI0_DEV_FAIL = 0x05,
    USB2_DEV_FAIL = 0x06,
    OCPRC_DEV_FAIL = 0x07,
    BOOT_CHAIN_EXHAUST = 0x08,
    BOOT_IMAGE_LOAD_FAIL = 0x09,
    DOT_SLOT_EXHAUST = 0x0A,
    S2A_HEADER_CHECK_FAIL = 0x0B,
    S2A_SANITY_FAIL = 0x0C,
    S2A_INTEGRITY_FAIL = 0x0D,
    S2A_KEY_REVOKED = 0x0E,
    MUTABLE_DOT_HEADER_CHECK_FAIL = 0x0F,
    MUTABLE_DOT_INTEGRITY_FAIL = 0x10,
    MUTABLE_DOT_SANITY_FAIL = 0x11,
    VOLATILE_DOT_PARITY_FAIL = 0x12,
    VOLATILE_DOT_HEADER_CHECK_FAIL = 0x13,
    VOLATILE_DOT_SANITY_FAIL = 0x14,
    CALIPTRA_ACK_FAIL = 0x15,
    FMC_CSH_SANITY_FAIL = 0x16,
    FMC_CSH_AUTHZ1_FAIL = 0x17,
    FMC_CSH_AUTHZ2_FAIL = 0x18,
    FMC_CSH_BIN_LIST_INTEGRITY_FAIL = 0x19,
    FMC_IMAGE_SANITY_FAIL = 0x1A,
    FMC_IMAGE_INTEGRITY_FAIL = 0x1B,
    FMC_IMAGE_DECRYPTION_FAIL = 0x1C,
    FMC_IMAGE_OEM_RATCHET_FAIL = 0x1D,
    FMC_IMAGE_MUT_DOT_SVN_FUSE_RATCHET_FAIL = 0x1E,
    FMC_IMAGE_MUT_DOT_SVN_CSH_RATCHET_FAIL = 0x1F,
    FMC_IMAGE_VOL_DOT_SVN_FUSE_RATCHET_FAIL = 0x20,
    FMC_IMAGE_VOL_DOT_SVN_CSH_RATCHET_FAIL = 0x21,
    FMC_IMAGE_VOL_DOT_CSH_RATCHET_FAIL = 0x22
};

// PSC FMC Progress Codes (Status Code operation field, Bits 5:0)
enum class PscFmcProgressCode : uint8_t
{
    INIT = 0x01,
    BOOT_MODE = 0x02,
    LPI_LS_LINK_UP = 0x03,
    FW_QSPI_REINIT = 0x04,
    BOOT_CHAIN_LEDGER_SELECT = 0x05,
    DATA_QSPI_INIT = 0x06,
    DEBUG_TOKEN_LOAD = 0x07,
    BINARY_LOAD = 0x08,
    BOOTSTRAP = 0x09,
    WAIT_FOR_DOT_CAK_STATUS = 0x0A,
    CSA = 0x0B,
    PLDM_T5_READY = 0x0C
};

// PSC FMC Error Codes (Status Code operation field, Bits 5:0)
enum class PscFmcErrorCode : uint8_t
{
    FUSE_CRC_FAILED = 0x01,
    LPI_LS_LINK_FAILED = 0x02,
    BOOT_CHAIN_LEDGER_INVALID = 0x03,
    BOOT_CHAIN_LEDGER_NOT_BOOTABLE = 0x04,
    BOOT_CHAIN_LEDGER_MISMATCH = 0x05,
    DEBUG_TOKEN_SANITY_FAIL = 0x06,
    DEBUG_TOKEN_AUTHENTICATION_FAIL = 0x07,
    SANITY_FAILED = 0x08,
    STAGE1_AUTHENTICATION_FAILED = 0x09,
    STAGE2_AUTHENTICATION_FAILED = 0x0A,
    SVN_CHECK_FAILED = 0x0B,
    HALT_DISABLED_SOCKET = 0x0C,
    MEM_FUSE_CRC_FAILED = 0x0D,
    CALIPTRA_MAILBOX_FAILED = 0x0E,
    CSA_FAILED = 0x0F,
    DOT_FAILED = 0x10
};

// BOOT_SEL_VAL decoding (encoded in bits 6:0 for BOOT_MODE_SEL_DONE)
// Boot mode (bit 0)
enum class BootMode : uint8_t
{
    Recovery = 0,
    ColdbootOrStreaming = 1
};

// Boot device (bits 1-2)
enum class BootDevice : uint8_t
{
    Invalid = 0,
    QSPI = 1,
    OCP_RC = 2,
    USB = 3
};

// Device disabled flag (bit 3)
enum class DeviceState : uint8_t
{
    Enabled = 0,
    Disabled = 1
};

// Selection type (bits 4-5)
enum class SelectionType : uint8_t
{
    STRAP = 0,
    OCP_CMD = 1,
    FUSE = 2,
    BCR = 3
};

// Recovery type (bit 6)
enum class RecoveryType : uint8_t
{
    ErrorInitiated = 0,
    Forced = 1
};

// BOOT_SEL_VAL information structure
struct BootSelInfo
{
    BootMode bootMode;
    BootDevice bootDevice;
    DeviceState deviceState;
    SelectionType selectionType;
    RecoveryType recoveryType;
    std::string stringRepresentation;
};

// Core progress code information structure
struct ProgressCodeInfo
{
    CodeType type;      // Progress, Error, or Debug
    PackageClass cpuId; // CPU package (Package0, Package1, or Unknown)
    SubClass subClass;  // Component/subsystem that generated the code
    uint8_t operation;  // Operation code (bits 5:0)
    std::string name;   // Human-readable string for the progress code

    // Boot selection information (only present when progress code is
    // PSC_ROM_PC_BOOT_MODE_SEL_DONE)
    std::optional<BootSelInfo> bootSelectionInfo;

    std::string getBootSelectionString() const
    {
        return bootSelectionInfo ? bootSelectionInfo->stringRepresentation : "";
    }
};

// Recovery complete codes
namespace RecoveryCompleteCodes
{
constexpr uint32_t CPU0 = 0x70C1C788;
constexpr uint32_t CPU1 = 0x71C1C788;
} // namespace RecoveryCompleteCodes

// Lookup tables for PSC ROM codes
namespace Detail
{
// PSC ROM error reason code lookup table
inline constexpr std::string_view psc_rom_error_reasons[] = {
    "",                                                   // 0x00
    "PSC_ROM_EC_I2C_EXT_MSG_FAIL",                        // 0x01
    "PSC_ROM_EC_BOOT_MODE_SEL_FAIL",                      // 0x02
    "PSC_ROM_EC_USB_EXT_MSG_FAIL",                        // 0x03
    "PSC_ROM_EC_EROT_GRANT_FAIL",                         // 0x04
    "PSC_ROM_EC_QSPI0_DEV_FAIL",                          // 0x05
    "PSC_ROM_EC_USB2_DEV_FAIL",                           // 0x06
    "PSC_ROM_EC_OCPRC_DEV_FAIL",                          // 0x07
    "PSC_ROM_EC_BOOT_CHAIN_EXHAUST",                      // 0x08
    "PSC_ROM_EC_BOOT_IMAGE_LOAD_FAIL",                    // 0x09
    "PSC_ROM_EC_DOT_SLOT_EXHAUST",                        // 0x0A
    "PSC_ROM_EC_S2A_HEADER_CHECK_FAIL",                   // 0x0B
    "PSC_ROM_EC_S2A_SANITY_FAIL",                         // 0x0C
    "PSC_ROM_EC_S2A_INTEGRITY_FAIL",                      // 0x0D
    "PSC_ROM_EC_S2A_KEY_REVOKED",                         // 0x0E
    "PSC_ROM_EC_MUTABLE_DOT_HEADER_CHECK_FAIL",           // 0x0F
    "PSC_ROM_EC_MUTABLE_DOT_INTEGRITY_FAIL",              // 0x10
    "PSC_ROM_EC_MUTABLE_DOT_SANITY_FAIL",                 // 0x11
    "PSC_ROM_EC_VOLATILE_DOT_PARITY_FAIL",                // 0x12
    "PSC_ROM_EC_VOLATILE_DOT_HEADER_CHECK_FAIL",          // 0x13
    "PSC_ROM_EC_VOLATILE_DOT_SANITY_FAIL",                // 0x14
    "PSC_ROM_EC_CALIPTRA_ACK_FAIL",                       // 0x15
    "PSC_ROM_EC_FMC_CSH_SANITY_FAIL",                     // 0x16
    "PSC_ROM_EC_FMC_CSH_AUTHZ1_FAIL",                     // 0x17
    "PSC_ROM_EC_FMC_CSH_AUTHZ2_FAIL",                     // 0x18
    "PSC_ROM_EC_FMC_CSH_BIN_LIST_INTEGRITY_FAIL",         // 0x19
    "PSC_ROM_EC_FMC_IMAGE_SANITY_FAIL",                   // 0x1A
    "PSC_ROM_EC_FMC_IMAGE_INTEGRITY_FAIL",                // 0x1B
    "PSC_ROM_EC_FMC_IMAGE_DECRYPTION_FAIL",               // 0x1C
    "PSC_ROM_EC_FMC_IMAGE_OEM_RATCHET_FAIL",              // 0x1D
    "PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_FUSE_RATCHET_FAIL", // 0x1E
    "PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_CSH_RATCHET_FAIL",  // 0x1F
    "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_FUSE_RATCHET_FAIL", // 0x20
    "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_CSH_RATCHET_FAIL",  // 0x21
    "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_CSH_RATCHET_FAIL"       // 0x22
};

inline constexpr std::size_t PSC_ROM_ERROR_COUNT =
    std::size(psc_rom_error_reasons);

// PSC ROM progress code lookup table
inline constexpr std::string_view psc_rom_progress_names[] = {
    "",                                   // 0x00
    "PSC_ROM_PC_I2C_EXT_MSG_INIT",        // 0x01
    "PSC_ROM_PC_BOOT_MODE_SEL_DONE",      // 0x02
    "PSC_ROM_PC_USB_EXT_MSG_INIT",        // 0x03
    "PSC_ROM_PC_QSPI0_DEV_INIT",          // 0x04
    "PSC_ROM_PC_USB2_DEV_INIT",           // 0x05
    "PSC_ROM_PC_OCPRC_DEV_INIT",          // 0x06
    "PSC_ROM_PC_BOOT_CHAIN_SEL",          // 0x07
    "PSC_ROM_PC_BOOT_IMAGE_LOAD_DONE",    // 0x08
    "PSC_ROM_PC_DOT_S2A_VALIDATION_DONE", // 0x09
    "PSC_ROM_PC_FMC_VALIDATION_DONE",     // 0x0A
    "PSC_ROM_PC_ROM_EXIT",                // 0x0B
    "PSC_ROM_PC_DOT_RECOVERY"             // 0x0C
};

inline constexpr std::size_t PSC_ROM_PROGRESS_COUNT =
    std::size(psc_rom_progress_names);

// PSC FMC progress code lookup table
inline constexpr std::string_view psc_fmc_progress_names[] = {
    "",                                    // 0x00
    "PSC_FMC_PC_INIT",                     // 0x01
    "PSC_FMC_PC_BOOT_MODE",                // 0x02
    "PSC_FMC_PC_LPI_LS_LINK_UP",           // 0x03
    "PSC_FMC_PC_FW_QSPI_REINIT",           // 0x04
    "PSC_FMC_PC_BOOT_CHAIN_LEDGER_SELECT", // 0x05
    "PSC_FMC_PC_DATA_QSPI_INIT",           // 0x06
    "PSC_FMC_PC_DEBUG_TOKEN_LOAD",         // 0x07
    "PSC_FMC_PC_BINARY_LOAD",              // 0x08
    "PSC_FMC_PC_BOOTSTRAP",                // 0x09
    "PSC_FMC_PC_WAIT_FOR_DOT_CAK_STATUS",  // 0x0A
    "PSC_FMC_PC_CSA",                      // 0x0B
    "PSC_FMC_PC_PLDM_T5_READY"             // 0x0C
};

inline constexpr std::size_t PSC_FMC_PROGRESS_COUNT =
    std::size(psc_fmc_progress_names);

// PSC FMC error code lookup table
inline constexpr std::string_view psc_fmc_error_reasons[] = {
    "",                                           // 0x00
    "PSC_FMC_EC_FUSE_CRC_FAILED",                 // 0x01
    "PSC_FMC_EC_LPI_LS_LINK_FAILED",              // 0x02
    "PSC_FMC_EC_BOOT_CHAIN_LEDGER_INVALID",       // 0x03
    "PSC_FMC_EC_BOOT_CHAIN_LEDGER_NOT_BOOTABLE",  // 0x04
    "PSC_FMC_EC_BOOT_CHAIN_LEDGER_MISMATCH",      // 0x05
    "PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL",         // 0x06
    "PSC_FMC_EC_DEBUG_TOKEN_AUTHENTICATION_FAIL", // 0x07
    "PSC_FMC_EC_SANITY_FAILED",                   // 0x08
    "PSC_FMC_EC_STAGE1_AUTHENTICATION_FAILED",    // 0x09
    "PSC_FMC_EC_STAGE2_AUTHENTICATION_FAILED",    // 0x0A
    "PSC_FMC_EC_SVN_CHECK_FAILED",                // 0x0B
    "PSC_FMC_EC_HALT_DISABLED_SOCKET",            // 0x0C
    "PSC_FMC_EC_MEM_FUSE_CRC_FAILED",             // 0x0D
    "PSC_FMC_EC_CALIPTRA_MAILBOX_FAILED",         // 0x0E
    "PSC_FMC_EC_CSA_FAILED",                      // 0x0F
    "PSC_FMC_EC_DOT_FAILED"                       // 0x10
};

inline constexpr std::size_t PSC_FMC_ERROR_COUNT =
    std::size(psc_fmc_error_reasons);
} // namespace Detail

/**
 * @brief Convert CodeType enum to string
 * @param type CodeType enum value
 * @return String representation of the code type
 */
constexpr std::string_view codeTypeToString(CodeType type)
{
    using namespace std::string_view_literals;
    switch (type)
    {
        case CodeType::Progress:
            return "Progress"sv;
        case CodeType::Error:
            return "Error"sv;
        case CodeType::Debug:
            return "Debug"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert PackageClass enum to string
 * @param pkg PackageClass enum value
 * @return String representation of the package class
 */
constexpr std::string_view packageClassToString(PackageClass pkg)
{
    using namespace std::string_view_literals;
    switch (pkg)
    {
        case PackageClass::Package0:
            return "CPU0"sv;
        case PackageClass::Package1:
            return "CPU1"sv;
        case PackageClass::Unknown:
            return "Unknown"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert SubClass enum to string
 * @param subclass SubClass enum value
 * @return String representation of the subclass
 */
constexpr std::string_view subClassToString(SubClass subclass)
{
    using namespace std::string_view_literals;
    switch (subclass)
    {
        case SubClass::PSC_ROM:
            return "PSC_ROM"sv;
        case SubClass::PSC_FMC:
            return "PSC_FMC"sv;
        case SubClass::PSC_RT:
            return "PSC_RT"sv;
        case SubClass::MB1:
            return "MB1"sv;
        case SubClass::BPMP_FW:
            return "BPMP_FW"sv;
        case SubClass::MB2:
            return "MB2"sv;
        case SubClass::ATF_BL31:
            return "ATF_BL31"sv;
        case SubClass::RMM:
            return "RMM"sv;
        case SubClass::Hafnium:
            return "Hafnium"sv;
        case SubClass::UEFI:
            return "UEFI"sv;
        case SubClass::UEFI_StMM:
            return "UEFI_StMM"sv;
        case SubClass::OOBHUB_FW:
            return "OOBHUB_FW"sv;
        case SubClass::RAS_FW:
            return "RAS_FW"sv;
        case SubClass::MSEQ_FW:
            return "MSEQ_FW"sv;
        case SubClass::PCORE0_FW:
            return "PCORE0_FW"sv;
        case SubClass::PCORE1_FW:
            return "PCORE1_FW"sv;
        case SubClass::PCORE2_FW:
            return "PCORE2_FW"sv;
        case SubClass::PCORE3_FW:
            return "PCORE3_FW"sv;
        case SubClass::PCORE4_FW:
            return "PCORE4_FW"sv;
        case SubClass::PCORE5_FW:
            return "PCORE5_FW"sv;
        case SubClass::C2C_GRS_0:
            return "C2C_GRS_0"sv;
        case SubClass::C2C_GRS_1:
            return "C2C_GRS_1"sv;
        case SubClass::C2C_UPHY_0:
            return "C2C_UPHY_0"sv;
        case SubClass::C2C_UPHY_1:
            return "C2C_UPHY_1"sv;
        case SubClass::C2C_UPHY_2:
            return "C2C_UPHY_2"sv;
        case SubClass::C2C_UPHY_3:
            return "C2C_UPHY_3"sv;
        case SubClass::C2C_UPHY_4:
            return "C2C_UPHY_4"sv;
        case SubClass::C2C_UPHY_5:
            return "C2C_UPHY_5"sv;
        case SubClass::C2C_LPI_S_0:
            return "C2C_LPI_S_0"sv;
        case SubClass::C2C_LPI_S_1:
            return "C2C_LPI_S_1"sv;
        case SubClass::C2C_LPI_C_0:
            return "C2C_LPI_C_0"sv;
        case SubClass::C2C_LPI_C_1:
            return "C2C_LPI_C_1"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert BootMode enum to string
 * @param mode BootMode enum value
 * @return String representation of boot mode
 */
constexpr std::string_view bootModeToString(BootMode mode)
{
    using namespace std::string_view_literals;
    switch (mode)
    {
        case BootMode::Recovery:
            return "Recovery"sv;
        case BootMode::ColdbootOrStreaming:
            return "Coldboot/Streaming"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert BootDevice enum to string
 * @param device BootDevice enum value
 * @return String representation of boot device
 */
constexpr std::string_view bootDeviceToString(BootDevice device)
{
    using namespace std::string_view_literals;
    switch (device)
    {
        case BootDevice::Invalid:
            return "Invalid"sv;
        case BootDevice::QSPI:
            return "QSPI"sv;
        case BootDevice::OCP_RC:
            return "OCP_RC"sv;
        case BootDevice::USB:
            return "USB"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert SelectionType enum to string
 * @param type SelectionType enum value
 * @return String representation of selection type
 */
constexpr std::string_view selectionTypeToString(SelectionType type)
{
    using namespace std::string_view_literals;
    switch (type)
    {
        case SelectionType::STRAP:
            return "STRAP"sv;
        case SelectionType::OCP_CMD:
            return "OCP_CMD"sv;
        case SelectionType::FUSE:
            return "FUSE"sv;
        case SelectionType::BCR:
            return "BCR"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Convert RecoveryType enum to string
 * @param type RecoveryType enum value
 * @return String representation of recovery type
 */
constexpr std::string_view recoveryTypeToString(RecoveryType type)
{
    using namespace std::string_view_literals;
    switch (type)
    {
        case RecoveryType::ErrorInitiated:
            return "Error-Initiated"sv;
        case RecoveryType::Forced:
            return "Forced"sv;
    }
    return "Unknown"sv;
}

/**
 * @brief Extract code type from progress code
 * @param progressCode 32-bit progress code
 * @return CodeType enum (bits 31:30)
 */
constexpr CodeType getCodeType(uint32_t progressCode)
{
    return static_cast<CodeType>((progressCode >> 30) & 0x3);
}

/**
 * @brief Extract sub-class from progress code
 * @param progressCode 32-bit progress code
 * @return SubClass enum (bits 23:16)
 */
constexpr SubClass getSubClass(uint32_t progressCode)
{
    return static_cast<SubClass>((progressCode >> 16) & 0xFF);
}

/**
 * @brief Extract operation code from progress code
 * @param progressCode 32-bit progress code
 * @return Operation code (bits 5:0)
 */
constexpr uint8_t getOperation(uint32_t progressCode)
{
    return progressCode & 0x3F;
}

/**
 * @brief Extract BOOT_SEL_VAL from progress code
 * @param progressCode 32-bit progress code
 * @return BOOT_SEL_VAL value (bits 12:6, 7 bits total)
 * @note Only meaningful for PSC_ROM_PC_BOOT_MODE_SEL_DONE (SubClass=PSC_ROM,
 * Operation=0x02)
 */
constexpr uint16_t getBootSelVal(uint32_t progressCode)
{
    return (progressCode >> 6) & 0x7F; // Extract bits 12:6
}

/**
 * @brief Decode BOOT_SEL_VAL into structured information with cached string
 * representation
 * @param bootSelVal 7-bit BOOT_SEL_VAL value
 * @return BootSelInfo structure with all fields including cached string
 * @note bootSelVal is meaningful when the progress code is from
 * PSC_ROM_PC_BOOT_MODE_SEL_DONE
 */
inline BootSelInfo decodeBootSelVal(uint16_t bootSelVal)
{
    const auto bootMode = static_cast<BootMode>(bootSelVal & 0x01);
    const auto bootDevice = static_cast<BootDevice>((bootSelVal >> 1) & 0x03);
    const auto deviceState = static_cast<DeviceState>((bootSelVal >> 3) & 0x01);
    const auto selectionType =
        static_cast<SelectionType>((bootSelVal >> 4) & 0x03);
    const auto recoveryType =
        static_cast<RecoveryType>((bootSelVal >> 6) & 0x01);

    return BootSelInfo{
        .bootMode = bootMode,
        .bootDevice = bootDevice,
        .deviceState = deviceState,
        .selectionType = selectionType,
        .recoveryType = recoveryType,
        .stringRepresentation = std::format(
            "Boot mode: {}, Boot device: {}, Device Disabled flag: {}, Selection type: {}, Recovery type: {}",
            bootModeToString(bootMode), bootDeviceToString(bootDevice),
            deviceState == DeviceState::Enabled ? "Enabled" : "Disabled",
            selectionTypeToString(selectionType),
            bootMode == BootMode::Recovery ? recoveryTypeToString(recoveryType)
                                           : "")};
}

/**
 * @brief Extract CPU package class from progress code
 * @param progressCode 32-bit progress code
 * @return PackageClass enum (Package0, Package1, or Unknown)
 */
constexpr PackageClass extractCpuId(uint32_t progressCode)
{
    const uint8_t pkgClass = (progressCode >> 24) & 0x3F;

    if (pkgClass == static_cast<uint8_t>(PackageClass::Package0))
    {
        return PackageClass::Package0;
    }
    if (pkgClass == static_cast<uint8_t>(PackageClass::Package1))
    {
        return PackageClass::Package1;
    }
    return PackageClass::Unknown;
}

/**
 * @brief Convert PackageClass to logical CPU ID
 * @param pkg PackageClass enum value
 * @return CPU ID (0, 1, or 255 for unknown)
 */
constexpr uint8_t packageClassToCpuId(PackageClass pkg)
{
    switch (pkg)
    {
        case PackageClass::Package0:
            return 0;
        case PackageClass::Package1:
            return 1;
        case PackageClass::Unknown:
            return 255;
    }
    return 255;
}

/**
 * @brief Check if progress code indicates recovery complete
 * @param progressCode 32-bit progress code
 * @return true if code indicates recovery is complete
 */
constexpr bool isRecoveryComplete(uint32_t progressCode)
{
    return (progressCode == RecoveryCompleteCodes::CPU0) ||
           (progressCode == RecoveryCompleteCodes::CPU1);
}

/**
 * @brief Map operation code to human-readable string
 * @param codeType Type of code (Progress or Error)
 * @param subClass Subclass (PSC_ROM, PSC_FMC, etc.)
 * @param operation Operation code (bits 5:0)
 * @return Operation description string
 */
inline std::string mapOperationToString(CodeType codeType, SubClass subClass,
                                        uint8_t operation)
{
    if (subClass == SubClass::PSC_ROM)
    {
        if (codeType == CodeType::Error)
        {
            if (operation < Detail::PSC_ROM_ERROR_COUNT &&
                !Detail::psc_rom_error_reasons[operation].empty())
            {
                return std::string(Detail::psc_rom_error_reasons[operation]);
            }
        }
        else if (codeType == CodeType::Progress)
        {
            if (operation < Detail::PSC_ROM_PROGRESS_COUNT &&
                !Detail::psc_rom_progress_names[operation].empty())
            {
                return std::string(Detail::psc_rom_progress_names[operation]);
            }
        }
    }
    else if (subClass == SubClass::PSC_FMC)
    {
        if (codeType == CodeType::Error)
        {
            if (operation < Detail::PSC_FMC_ERROR_COUNT &&
                !Detail::psc_fmc_error_reasons[operation].empty())
            {
                return std::string(Detail::psc_fmc_error_reasons[operation]);
            }
        }
        else if (codeType == CodeType::Progress)
        {
            if (operation < Detail::PSC_FMC_PROGRESS_COUNT &&
                !Detail::psc_fmc_progress_names[operation].empty())
            {
                return std::string(Detail::psc_fmc_progress_names[operation]);
            }
        }
    }
    return ""; // Return empty string for unmapped codes
}

/**
 * @brief Get detailed progress code information
 * @param progressCode 32-bit progress code
 * @return ProgressCodeInfo struct with information on the progress code
 */
inline ProgressCodeInfo getProgressCodeInfo(uint32_t progressCode)
{
    ProgressCodeInfo info;
    info.type = getCodeType(progressCode);
    info.subClass = getSubClass(progressCode);
    info.cpuId = extractCpuId(progressCode);
    info.operation = getOperation(progressCode);

    // Map operation to string for all other codes
    info.name = mapOperationToString(info.type, info.subClass, info.operation);

    info.bootSelectionInfo = std::nullopt;

    // Populate bootSelectionInfo if BOOT_MODE_SEL_DONE progress code
    if (info.type == CodeType::Progress && info.subClass == SubClass::PSC_ROM &&
        info.operation ==
            static_cast<uint8_t>(PscRomProgressCode::BOOT_MODE_SEL_DONE))
    {

        // Decode boot selection info first to check boot mode
        info.bootSelectionInfo = decodeBootSelVal(getBootSelVal(progressCode));
        return info;
    }

    return info;
}

} // namespace ProgressCodeParser
