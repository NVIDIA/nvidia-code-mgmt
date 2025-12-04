#pragma once

#include "ecid_parser.hpp"

#include <libusb-1.0/libusb.h>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

/**
 * @file usb_io.hpp
 * @brief USB register I/O via libusb control transfers
 *
 * @note Register values are little-endian
 * @note Caller must initialize libusb context
 */

namespace usb_io
{

/**
 * @brief Read 32-bit register from device virtual address space
 *
 * @param handle USB device handle (must be valid and open)
 * @param virtualAddress Virtual address to read from (full 32-bit address
 * space)
 * @param verbose Enable verbose error logging to stderr (default: false)
 * @return Register value on success, std::nullopt on failure
 *
 * @par Example:
 * @code
 * if (auto value = usb_io::readRegister32(handle, 0x8000, true)) {
 *   std::cout << "Register value: 0x" << std::hex << *value << '\n';
 * }
 * @endcode
 */
[[nodiscard]] std::optional<uint32_t>
    readRegister32(libusb_device_handle* handle, uint32_t virtualAddress,
                   bool verbose = false) noexcept;

/**
 * @brief Read ECID from USB device serial number descriptor
 *
 * Reads the device serial number descriptor and parses it as a 256-bit ECID.
 * The serial number must be a 64-character hex string representing 32 bytes.
 *
 * @param handle USB device handle (must be valid and open)
 * @param verbose Enable verbose error logging to stderr (default: false)
 * @return ECID array on success, std::nullopt on failure
 *
 * @par Example:
 * @code
 * if (auto ecid = usb_io::readDeviceEcid(handle, true)) {
 *   bool needsDotBlob = EcidParser::isDOTBlobRequired(ecid->data());
 * }
 * @endcode
 */
[[nodiscard]] std::optional<std::array<uint8_t, EcidParser::ECID_SIZE>>
    readDeviceEcid(libusb_device_handle* handle, bool verbose = false) noexcept;

} // namespace usb_io
