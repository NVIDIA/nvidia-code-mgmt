#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

/**
 * @brief ECID Structure for Vera (256-bit UID)
 * - Bytes  0-15:  Device Serial Info
 * - Bytes 16-19:  ECID4 - Boot NV Info  
 * - Bytes 20-23:  ECID5 - Contains OPT_OWNERSHIP_STATUS
 * - Bytes 24-27:  ECID6 - PSC_BL Ratchet
 * - Bytes 28-31:  ECID7 - Contains BSI_OEM_KEY_VALID
 */

namespace EcidParser {

// ECID size constant
inline constexpr std::size_t ECID_SIZE = 32;

// Blob requirement types (priority: S2A > DOT > None)
enum class BlobRequirement : uint8_t {
    None,      // Standard images only
    DOTBlob,   // DOT blob required (Mutable DOT State)
    S2ABlob    // S2A blob required (OEM key valid, not supported)
};

// Bit field locations
namespace BitField {
    inline constexpr std::size_t BSI_OEM_KEY_VALID_BYTE = 28;
    inline constexpr uint8_t BSI_OEM_KEY_VALID_MASK = 0x02;
    inline constexpr std::size_t OPT_OWNERSHIP_STATUS_BYTE_LOW = 20;
    inline constexpr std::size_t OPT_OWNERSHIP_STATUS_BYTE_HIGH = 21;
    inline constexpr uint16_t OPT_OWNERSHIP_STATUS_MASK = 0x1FF;
    inline constexpr uint16_t OPT_OWNERSHIP_ODD_MASK = 0x01;
} // namespace BitField

/**
 * @brief Extract BSI_OEM_KEY_VALID bit from ECID
 * @param ecid Pointer to 32-byte ECID array
 * @return true if OEM key is valid (bit 1 of byte 28 is set)
 */
constexpr bool isOemKeyValid(const uint8_t* ecid) {
    return (ecid[BitField::BSI_OEM_KEY_VALID_BYTE] & BitField::BSI_OEM_KEY_VALID_MASK) != 0;
}

/**
 * @brief Extract OPT_OWNERSHIP_STATUS from ECID
 * @param ecid Pointer to 32-byte ECID array
 * @return 9-bit ownership status value (bits 168:160)
 */
constexpr uint16_t getOwnershipStatus(const uint8_t* ecid) {
    const uint16_t ecid5 = 
        (static_cast<uint16_t>(ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_HIGH]) << 8) |
         static_cast<uint16_t>(ecid[BitField::OPT_OWNERSHIP_STATUS_BYTE_LOW]);
    return ecid5 & BitField::OPT_OWNERSHIP_STATUS_MASK;
}

/**
 * @brief Check if DOT blob is required based on ECID fuses
 * @param ecid Pointer to 32-byte ECID array
 * @return true if DOT blob is required
 */
constexpr bool isDOTBlobRequired(const uint8_t* ecid) {
    if (isOemKeyValid(ecid)) {
        return false;
    }
    const auto ownership_status = getOwnershipStatus(ecid);
    return (ownership_status & BitField::OPT_OWNERSHIP_ODD_MASK) != 0;
}

/**
 * @brief Check if S2A blob is required based on ECID fuses
 * @param ecid Pointer to 32-byte ECID array
 * @return true if S2A blob is required
 */
constexpr bool isS2ABlobRequired(const uint8_t* ecid) {
    return isOemKeyValid(ecid);
}

/**
 * @brief Determine blob requirement type (for programmatic use)
 * @param ecid Pointer to 32-byte ECID array
 * @return BlobRequirement enum (S2A > DOT > None priority)
 * @pre ecid must point to a valid 32-byte array
 */
constexpr BlobRequirement getBlobRequirement(const uint8_t* ecid) {
    if (isS2ABlobRequired(ecid)) {
        return BlobRequirement::S2ABlob;
    }
    if (isDOTBlobRequired(ecid)) {
        return BlobRequirement::DOTBlob;
    }
    return BlobRequirement::None;
}

/**
 * @brief Convert BlobRequirement enum to human-readable string
 * @param requirement BlobRequirement enum value
 * @return String view describing the blob requirement
 */
constexpr std::string_view blobRequirementToString(BlobRequirement requirement) {
    using namespace std::string_view_literals;
    
    switch (requirement) {
        case BlobRequirement::S2ABlob:
            return "S2A blob required - Not supported"sv;
        case BlobRequirement::DOTBlob:
            return "DOT blob required - Mutable DOT State"sv;
        case BlobRequirement::None:
            return ""sv;
    }
    return ""sv;
}

/**
 * @brief Get blob requirement as string (convenience function for tools)
 * @param ecid Pointer to 32-byte ECID array
 * @return String view describing the blob requirement
 * @pre ecid must point to a valid 32-byte array
 */
constexpr std::string_view getBlobRequirementString(const uint8_t* ecid) {
    return blobRequirementToString(getBlobRequirement(ecid));
}

} // namespace EcidParser
