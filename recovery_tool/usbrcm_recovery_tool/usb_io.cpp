/**
 * @file usb_io.cpp
 * @brief USB register I/O implementation
 */

#include "usb_io.hpp"

#include <array>
#include <charconv>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>

// Compile-time assertions for type safety
static_assert(sizeof(uint32_t) == 4, "uint32_t must be exactly 4 bytes");
static_assert(sizeof(uint8_t) == 1, "uint8_t must be exactly 1 byte");

namespace usb_io
{

namespace
{

/**
 * @brief Convert 4-byte little-endian buffer to uint32_t (constexpr)
 * @param data 4-byte buffer containing little-endian data
 * @return 32-bit value in host byte order
 */
constexpr uint32_t
    littleEndianToUint32(const std::array<uint8_t, 4>& data) noexcept
{
    // Reinterpret bytes as uint32_t (little-endian representation)
    uint32_t value = std::bit_cast<uint32_t>(data);

    // If host is big-endian, swap bytes to convert to host order
    if constexpr (std::endian::native == std::endian::big)
    {
        return std::byteswap(value);
    }
    return value; // Little-endian host: no conversion needed
}

// Compile-time verification of endian conversion
static_assert(littleEndianToUint32({0x78, 0x56, 0x34, 0x12}) == 0x12345678,
              "Little-endian conversion must be correct");

/**
 * @brief USB control transfer protocol constants for register I/O
 * @note These match the USB RCM specification for CPU
 */
constexpr uint8_t REQUEST_TYPE_READ = 0xC0; ///< Device-to-host, vendor-specific
constexpr uint8_t REQUEST_TYPE_WRITE =
    0x40;                                 ///< Host-to-device, vendor-specific
constexpr uint8_t REQUEST_CODE = 0x00;    ///< bRequest value (both R/W)
constexpr uint32_t TIMEOUT_MS = 5000;     ///< Control transfer timeout
constexpr size_t REGISTER_SIZE_BYTES = 4; ///< 32-bit register size

// Validate register size matches uint32_t
static_assert(REGISTER_SIZE_BYTES == 4, "Register size must match uint32_t");

} // anonymous namespace

std::optional<uint32_t> readRegister32(libusb_device_handle* handle,
                                       const uint32_t virtualAddress,
                                       const bool verbose) noexcept
{
    // Validate input parameter
    if (handle == nullptr)
    {
        if (verbose)
        {
            std::cerr << "usb_io::readRegister32: Invalid handle (nullptr)\n";
        }
        return std::nullopt;
    }

    // Prepare receive buffer
    std::array<uint8_t, REGISTER_SIZE_BYTES> rxBuffer{};

    // Perform USB control transfer
    // Split 32-bit address: wValue = [31:16], wIndex = [15:0]
    const int bytesRead = libusb_control_transfer(
        handle,
        REQUEST_TYPE_READ,                                      // bmRequestType
        REQUEST_CODE,                                           // bRequest
        static_cast<uint16_t>((virtualAddress >> 16) & 0xFFFF), // wValue (MSB)
        static_cast<uint16_t>(virtualAddress & 0xFFFF),         // wIndex (LSB)
        rxBuffer.data(),                                        // data buffer
        static_cast<uint16_t>(rxBuffer.size()),                 // wLength
        TIMEOUT_MS);                                            // timeout

    if (bytesRead == static_cast<int>(REGISTER_SIZE_BYTES))
    {
        const uint32_t value = littleEndianToUint32(rxBuffer);
        return value;
    }

    // Handle transfer failure
    if (verbose)
    {
        std::ostringstream oss;
        oss << "usb_io::readRegister32: Transfer failed at address 0x"
            << std::hex << std::setfill('0') << std::setw(8) << virtualAddress
            << std::dec << " - ";

        if (bytesRead < 0)
        {
            oss << libusb_error_name(bytesRead) << " ("
                << libusb_strerror(static_cast<libusb_error>(bytesRead)) << ")";
        }
        else
        {
            // Partial read
            oss << "Incomplete read (expected " << REGISTER_SIZE_BYTES
                << " bytes, got " << bytesRead << " bytes)";
        }

        std::cerr << oss.str() << "\n";
    }

    return std::nullopt;
}

std::optional<std::array<uint8_t, EcidParser::ECID_SIZE>>
    readDeviceEcid(libusb_device_handle* handle, bool verbose) noexcept
{
    if (!handle)
    {
        if (verbose)
        {
            std::cerr << "readDeviceEcid: Invalid device handle\n";
        }
        return std::nullopt;
    }

    // Get device descriptor to find serial string index
    // Note: libusb_get_device() returns borrowed reference, no cleanup needed
    libusb_device* const device = libusb_get_device(handle);
    libusb_device_descriptor desc{};
    const int descResult = libusb_get_device_descriptor(device, &desc);
    if (descResult < 0)
    {
        if (verbose)
        {
            // Use stream operators instead of std::format in noexcept function
            std::cerr << "readDeviceEcid: Failed to get device descriptor: "
                      << libusb_error_name(descResult) << "\n";
        }
        return std::nullopt;
    }

    if (desc.iSerialNumber == 0)
    {
        if (verbose)
        {
            std::cerr
                << "readDeviceEcid: Device has no serial number descriptor\n";
        }
        return std::nullopt;
    }

    // ECID is 32 bytes = 64 hex characters + null terminator
    constexpr size_t ECID_HEX_LENGTH = EcidParser::ECID_SIZE * 2;
    constexpr size_t SERIAL_BUFFER_SIZE = ECID_HEX_LENGTH + 1;

    // RAII buffer: stack-allocated, automatic cleanup, no memory leaks
    std::array<uint8_t, SERIAL_BUFFER_SIZE> serialBuffer{};
    const int serialResult = libusb_get_string_descriptor_ascii(
        handle, desc.iSerialNumber, serialBuffer.data(),
        static_cast<int>(serialBuffer.size()));

    if (serialResult < 0)
    {
        if (verbose)
        {
            // Use stream operators instead of std::format in noexcept function
            std::cerr << "readDeviceEcid: Failed to read serial descriptor: "
                      << libusb_error_name(serialResult) << "\n";
        }
        return std::nullopt;
    }

    if (static_cast<size_t>(serialResult) < ECID_HEX_LENGTH)
    {
        if (verbose)
        {
            std::cerr << "readDeviceEcid: Invalid ECID serial string length: "
                      << serialResult << " (expected " << ECID_HEX_LENGTH
                      << ")\n";
        }
        return std::nullopt;
    }

    // Parse hex string to binary ECID using modern C++23 std::from_chars
    // Use std::string_view for bounds-safe, zero-copy string operations
    std::array<uint8_t, EcidParser::ECID_SIZE> ecid{};
    const std::string_view hexView(
        reinterpret_cast<const char*>(serialBuffer.data()), ECID_HEX_LENGTH);

    for (size_t i = 0; i < EcidParser::ECID_SIZE; ++i)
    {
        const size_t hexOffset = i * 2;
        const char* const byteStart = hexView.data() + hexOffset;
        const char* const byteEnd = byteStart + 2;

        uint8_t byteValue = 0;
        const auto [ptr, ec] =
            std::from_chars(byteStart, byteEnd, byteValue, 16);

        if (ec != std::errc() || ptr != byteEnd)
        {
            if (verbose)
            {
                std::cerr
                    << "readDeviceEcid: Invalid hex character in ECID at position "
                    << hexOffset << "\n";
            }
            return std::nullopt;
        }

        ecid[i] = byteValue;
    }

    if (verbose)
    {
        std::cout << "readDeviceEcid: Successfully read ECID (32 bytes)\n";
    }

    return ecid;
}

} // namespace usb_io
