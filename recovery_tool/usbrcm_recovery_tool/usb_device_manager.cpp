/**
 * @file usb_devicemanager.cpp
 * @brief RAII USB device management implementation
 */

#include "usb_device_manager.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <memory>
#include <span>

namespace usb
{

namespace
{
// USB device constants
constexpr uint16_t VENDOR_ID = 0x0955;          // NVIDIA vendor ID
constexpr uint16_t PRODUCT_ID = 0x7410;         // RCM mode product ID
constexpr uint8_t ENDPOINT_RECOVERY = 0x08;     // Recovery endpoint
constexpr uint8_t ENDPOINT_ADDRESS_MASK = 0x7F; // Endpoint number mask
constexpr size_t MAX_PORT_NUMBERS = 8;          // Maximum USB port depth

} // anonymous namespace

// ============================================================================
// UsbContext Implementation (RAII wrapper for libusb_context*)
// ============================================================================

UsbContext::UsbContext() noexcept
{
    const int result = libusb_init(&context);
    if (result != LIBUSB_SUCCESS)
    {
        std::cerr << std::format(
            "Failed to initialize libusb: {} ({})\n", libusb_error_name(result),
            libusb_strerror(static_cast<libusb_error>(result)));
        context = nullptr;
        return;
    }

    // Set logging level to WARNING
    libusb_set_option(context, LIBUSB_OPTION_LOG_LEVEL,
                      LIBUSB_LOG_LEVEL_WARNING);
}

UsbContext::~UsbContext() noexcept
{
    if (context)
    {
        libusb_exit(context);
        context = nullptr;
    }
}

UsbContext& UsbContext::operator=(UsbContext&& other) noexcept
{
    if (this != &other)
    {
        // Clean up our current context
        if (context)
        {
            libusb_exit(context);
        }

        // Take ownership from other
        context = other.context;
        other.context = nullptr;
    }
    return *this;
}

// ============================================================================
// UsbDevice Implementation (RAII wrapper for libusb_device*)
// ============================================================================

UsbDevice::UsbDevice(libusb_device* device, std::string portPath) noexcept :
    device(device), portPath(std::move(portPath))
{}

UsbDevice::~UsbDevice() noexcept
{
    if (device)
    {
        libusb_unref_device(device);
    }
}

UsbDevice::UsbDevice(UsbDevice&& other) noexcept :
    device(other.device), portPath(std::move(other.portPath))
{
    other.device = nullptr;
    other.portPath.clear();
}

UsbDevice& UsbDevice::operator=(UsbDevice&& other) noexcept
{
    if (this != &other)
    {
        if (device)
        {
            libusb_unref_device(device);
        }
        device = other.device;
        portPath = std::move(other.portPath);
        other.device = nullptr;
        other.portPath.clear();
    }
    return *this;
}

// ============================================================================
// Internal Helper Functions
// ============================================================================

namespace
{
/**
 * @brief Check if device matches target VID:PID (0x0955:0x7410)
 */
[[nodiscard]] bool hasCorrectVidPid(libusb_device* device,
                                    const bool verbose) noexcept
{
    if (!device)
    {
        return false;
    }

    libusb_device_descriptor desc{};
    const int ret = libusb_get_device_descriptor(device, &desc);
    if (ret != LIBUSB_SUCCESS)
    {
        if (verbose)
        {
            std::cerr << std::format("Failed to get device descriptor: {}\n",
                                     libusb_error_name(ret));
        }
        return false;
    }

    const bool matches =
        (desc.idVendor == VENDOR_ID && desc.idProduct == PRODUCT_ID);

    if (verbose && !matches)
    {
        std::cout << std::format(
            "Device VID:PID = 0x{:04X}:0x{:04X} (expected 0x{:04X}:0x{:04X})\n",
            desc.idVendor, desc.idProduct, VENDOR_ID, PRODUCT_ID);
    }

    return matches;
}

/**
 * @brief Check if device has recovery endpoints
 */
[[nodiscard]] bool hasRecoveryEndpoints(libusb_device* device,
                                        const bool verbose) noexcept
{
    if (!device)
    {
        return false;
    }

    libusb_config_descriptor* config = nullptr;
    const int ret = libusb_get_active_config_descriptor(device, &config);
    if (ret != LIBUSB_SUCCESS)
    {
        if (verbose)
        {
            std::cerr << std::format("Failed to get config descriptor: {}\n",
                                     libusb_error_name(ret));
        }
        return false; // config not allocated on error, safe to return
    }

    // RAII guard for config descriptor - ensures cleanup on all exit paths
    auto configGuard = [](libusb_config_descriptor* p) {
        if (p)
        {
            libusb_free_config_descriptor(p);
        }
    };
    std::unique_ptr<libusb_config_descriptor, decltype(configGuard)> guard(
        config, configGuard);

    // Search for recovery interface and endpoint using modern C++
    const auto interfaces =
        std::span(config->interface, config->bNumInterfaces);

    for (const auto& interface : interfaces)
    {
        const auto altsettings =
            std::span(interface.altsetting, interface.num_altsetting);

        for (const auto& altsetting : altsettings)
        {
            if (altsetting.bInterfaceNumber == INTERFACE_RECOVERY)
            {
                const auto endpoints =
                    std::span(altsetting.endpoint, altsetting.bNumEndpoints);

                const bool found = std::any_of(
                    endpoints.begin(), endpoints.end(), [](const auto& ep) {
                        return (ep.bEndpointAddress & ENDPOINT_ADDRESS_MASK) ==
                               ENDPOINT_RECOVERY;
                    });

                if (found)
                {
                    return true; // guard cleanup happens automatically
                }
            }
        }
    }

    // Not found
    if (verbose)
    {
        std::cout << std::format(
            "Device does not have recovery interface {} with endpoint 0x{:02X}\n",
            INTERFACE_RECOVERY, ENDPOINT_RECOVERY);
    }

    return false; // guard cleanup happens automatically
}

/**
 * @brief Get device port path (internal helper)
 */
[[nodiscard]] std::string getDevicePortPath(libusb_device* device) noexcept
{
    if (!device)
    {
        return {};
    }

    try
    {
        std::array<uint8_t, MAX_PORT_NUMBERS> ports{};
        const int portCount = libusb_get_port_numbers(
            device, ports.data(), static_cast<int>(ports.size()));

        if (portCount <= 0)
        {
            return {};
        }

        const uint8_t busNumber = libusb_get_bus_number(device);
        std::string portPath = std::format("{}", busNumber);

        // Build port path string using modern range-based for (e.g., "1-1.3.4")
        const auto validPorts =
            std::span(ports.data(), static_cast<size_t>(portCount));

        for (size_t i = 0; const auto& port : validPorts)
        {
            portPath += std::format("{}{}", (i++ == 0 ? "-" : "."), port);
        }

        return portPath;
    }
    catch (...)
    {
        // std::format or std::string can throw - catch to honor noexcept
        return {};
    }
}
} // anonymous namespace

// ============================================================================
// UsbDeviceHandle Implementation (RAII wrapper for libusb_devicehandle*)
// ============================================================================

UsbDeviceHandle::UsbDeviceHandle(libusb_device* device, const UsbContext& ctx,
                                 int interfaceNum) noexcept :
    claimedInterface(interfaceNum)
{
    if (!device)
    {
        std::cerr << "Cannot open null USB device\n";
        return;
    }

    if (!ctx.isValid())
    {
        std::cerr << "Cannot open device with invalid USB context\n";
        return;
    }

    // Get device info before opening
    portPath = getDevicePortPath(device);

    // Open device
    int ret = libusb_open(device, &handle);
    if (ret != LIBUSB_SUCCESS)
    {
        if (portPath.empty())
        {
            std::cerr << std::format(
                "Failed to open USB device: {} ({})\n", libusb_error_name(ret),
                libusb_strerror(static_cast<libusb_error>(ret)));
        }
        else
        {
            std::cerr << std::format(
                "Failed to open USB device at port {}: {} ({})\n", portPath,
                libusb_error_name(ret),
                libusb_strerror(static_cast<libusb_error>(ret)));
        }
        handle = nullptr;
        return;
    }

    // Set configuration (configuration 1)
    ret = libusb_set_configuration(handle, 1);
    if (ret != LIBUSB_SUCCESS && ret != LIBUSB_ERROR_BUSY)
    {
        std::cerr << std::format("Warning: Failed to set configuration 1: {}\n",
                                 libusb_error_name(ret));
    }

    // Detach kernel driver if active on specified interface
    if (libusb_kernel_driver_active(handle, claimedInterface) == 1)
    {
        ret = libusb_detach_kernel_driver(handle, claimedInterface);
        if (ret != LIBUSB_SUCCESS)
        {
            std::cerr << std::format(
                "Warning: Failed to detach kernel driver from interface {}: {}\n",
                claimedInterface, libusb_error_name(ret));
        }
    }

    // Claim specified interface
    ret = libusb_claim_interface(handle, claimedInterface);
    if (ret != LIBUSB_SUCCESS)
    {
        std::cerr << std::format(
            "Failed to claim USB interface {}: {} ({})\n", claimedInterface,
            libusb_error_name(ret),
            libusb_strerror(static_cast<libusb_error>(ret)));
        cleanup();
        return;
    }
}

UsbDeviceHandle::~UsbDeviceHandle() noexcept
{
    cleanup();
}

UsbDeviceHandle& UsbDeviceHandle::operator=(UsbDeviceHandle&& other) noexcept
{
    if (this != &other)
    {
        cleanup();

        handle = other.handle;
        portPath = std::move(other.portPath);
        claimedInterface = other.claimedInterface;

        other.handle = nullptr;
        other.portPath.clear();
        other.claimedInterface = -1;
    }
    return *this;
}

void UsbDeviceHandle::cleanup() noexcept
{
    if (handle)
    {
        if (claimedInterface >= 0)
        {
            // Release interface - log errors but continue cleanup
            int ret = libusb_release_interface(handle, claimedInterface);
            if (ret != LIBUSB_SUCCESS && ret != LIBUSB_ERROR_NOT_FOUND)
            {
                // Log non-trivial errors (NOT_FOUND is normal if already
                // released)
                std::cerr << "Warning: Failed to release interface "
                          << claimedInterface << ": "
                          << libusb_strerror(static_cast<libusb_error>(ret))
                          << '\n';
            }

            // Restore kernel driver - log errors but continue cleanup
            ret = libusb_attach_kernel_driver(handle, claimedInterface);
            if (ret != LIBUSB_SUCCESS && ret != LIBUSB_ERROR_NOT_FOUND &&
                ret != LIBUSB_ERROR_NOT_SUPPORTED)
            {
                // Log non-trivial errors (NOT_FOUND/NOT_SUPPORTED are normal on
                // some platforms)
                std::cerr
                    << "Warning: Failed to reattach kernel driver for interface "
                    << claimedInterface << ": "
                    << libusb_strerror(static_cast<libusb_error>(ret)) << '\n';
            }
        }
        libusb_close(handle);
        handle = nullptr;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool isDeviceInRcmMode(libusb_device* device, const bool verbose) noexcept
{
    if (!device)
    {
        if (verbose)
        {
            std::cerr << "Cannot check null device\n";
        }
        return false;
    }

    return hasCorrectVidPid(device, verbose) &&
           hasRecoveryEndpoints(device, verbose);
}

std::vector<UsbDevice> findDevicesByVidPid(const UsbContext& ctx,
                                           const bool verbose) noexcept
{
    if (!ctx.isValid())
    {
        if (verbose)
        {
            std::cerr << "Invalid USB context\n";
        }
        return {};
    }

    libusb_device** deviceList = nullptr;
    const ssize_t deviceCount = libusb_get_device_list(ctx.get(), &deviceList);

    if (deviceCount < 0)
    {
        if (verbose)
        {
            std::cerr << std::format(
                "Failed to get USB device list: {}\n",
                libusb_error_name(static_cast<int>(deviceCount)));
        }
        return {}; // libusb_get_devicelist does NOT allocate list on error,
                   // safe to return
    }

    // RAII guard for deviceList - ensures cleanup even if exception occurs
    auto deviceListGuard = [](libusb_device** p) {
        if (p)
        {
            libusb_free_device_list(p, 1);
        }
    };
    std::unique_ptr<libusb_device*, decltype(deviceListGuard)> guard(
        deviceList, deviceListGuard);

    if (verbose)
    {
        std::cout << std::format(
            "Scanning {} USB devices for VID:PID=0x{:04X}:0x{:04X}...\n",
            deviceCount, VENDOR_ID, PRODUCT_ID);
    }

    std::vector<UsbDevice> matchingDevices;
    matchingDevices.reserve(8); // Typical: few devices

    for (ssize_t i = 0; i < deviceCount; i++)
    {
        if (hasCorrectVidPid(deviceList[i], verbose))
        {
            libusb_ref_device(
                deviceList[i]); // Increment ref count (caller owns reference)

            const std::string portPath = getDevicePortPath(deviceList[i]);
            matchingDevices.push_back({deviceList[i], portPath});

            if (verbose)
            {
                std::cout << std::format(
                    "Found device VID:PID=0x{:04X}:0x{:04X} at port {}\n",
                    VENDOR_ID, PRODUCT_ID, portPath);
            }
        }
    }

    // guard destructor will call libusb_free_devicelist automatically

    if (verbose)
    {
        std::cout << std::format(
            "Found {} device(s) with VID:PID=0x{:04X}:0x{:04X}\n",
            matchingDevices.size(), VENDOR_ID, PRODUCT_ID);
    }

    return matchingDevices; // guard cleanup happens here
}

std::optional<UsbDevice> findDeviceByPortPath(const UsbContext& ctx,
                                              const std::string_view portPath,
                                              const bool verbose) noexcept
{
    if (!ctx.isValid())
    {
        if (verbose)
        {
            std::cerr << "Invalid USB context\n";
        }
        return std::nullopt;
    }

    if (portPath.empty())
    {
        if (verbose)
        {
            std::cerr << "Port path cannot be empty\n";
        }
        return std::nullopt;
    }

    // Get all devices with matching VID:PID
    auto devices = findDevicesByVidPid(ctx, verbose);

    // Find the one with matching port path
    for (auto& device : devices)
    {
        if (device.getPortPath() == portPath)
        {
            if (verbose)
            {
                std::cout << std::format("Found device at port {}\n", portPath);
            }
            return std::move(device); // Transfer ownership
        }
    }

    if (verbose)
    {
        std::cerr << std::format("No device found at port {}\n", portPath);
    }

    return std::nullopt;
}

} // namespace usb
