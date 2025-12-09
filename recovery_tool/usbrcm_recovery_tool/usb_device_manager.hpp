#pragma once

#include <libusb-1.0/libusb.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file usb_device_manager.hpp
 * @brief USB device management for RCM recovery
 */

namespace usb
{

/**
 * @brief RAII wrapper for libusb context
 *
 * Manages libusb_context lifetime with automatic cleanup. Initializes libusb
 * on construction and cleans up all resources on destruction. Must remain valid
 * for the lifetime of any USB devices or handles created with it.
 */
class UsbContext
{
  public:
    UsbContext() noexcept;
    ~UsbContext() noexcept;

    UsbContext(const UsbContext&) = delete;
    UsbContext& operator=(const UsbContext&) = delete;

    UsbContext(UsbContext&& other) noexcept : context(other.context)
    {
        other.context = nullptr;
    }
    UsbContext& operator=(UsbContext&& other) noexcept;

    [[nodiscard]] bool isValid() const noexcept
    {
        return context != nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return isValid();
    }
    [[nodiscard]] libusb_context* get() const noexcept
    {
        return context;
    }

  private:
    libusb_context* context = nullptr;
};

/// USB interface constant for RCM image transfers
constexpr int INTERFACE_RECOVERY =
    3; ///< Recovery interface for RCM image transfers

/**
 * @brief Lightweight USB control session for control transfers
 *
 * Opens device but does NOT claim any interface.
 * Use for operations that only need endpoint 0 (control transfers):
 * - Progress code register reads
 * - ECID reads (string descriptors)
 *
 * For bulk/interrupt transfers that require claiming an interface,
 * use UsbDeviceHandle instead.
 */
class UsbControlSession
{
  public:
    /**
     * @brief Open USB control session for control transfers
     * @param device USB device to open
     * @param ctx USB context (must remain valid for control session lifetime)
     */
    UsbControlSession(libusb_device* device, const UsbContext& ctx) noexcept;
    ~UsbControlSession() noexcept;

    // Non-copyable
    UsbControlSession(const UsbControlSession&) = delete;
    UsbControlSession& operator=(const UsbControlSession&) = delete;

    // Movable
    UsbControlSession(UsbControlSession&& other) noexcept :
        handle(other.handle), portPath(std::move(other.portPath))
    {
        other.handle = nullptr;
        other.portPath.clear();
    }
    UsbControlSession& operator=(UsbControlSession&& other) noexcept;

    [[nodiscard]] bool isValid() const noexcept
    {
        return handle != nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return isValid();
    }
    [[nodiscard]] libusb_device_handle* get() const noexcept
    {
        return handle;
    }

    /**
     * @brief Get USB port path (e.g., "1-1.3")
     * @return Port path string cached at construction time
     */
    [[nodiscard]] const std::string& getPortPath() const noexcept
    {
        return portPath;
    }

  private:
    libusb_device_handle* handle = nullptr;
    std::string portPath;
};

/**
 * @brief RAII wrapper for USB device handle with interface claim
 *
 * Manages libusb_device_handle lifetime with automatic cleanup. Opens device,
 * claims interface on construction. Releases interface and closes device on
 * destruction. Also caches the USB port path at construction time for efficient
 * access.
 *
 * Use this class when you need to perform bulk/interrupt transfers on a
 * specific interface (e.g., RCM image transfers on INTERFACE_RECOVERY).
 * For control transfers only, prefer UsbControlSession instead.
 */
class UsbDeviceHandle
{
  public:
    /**
     * @brief Construct USB device handle and claim specified interface
     * @param device USB device to open
     * @param ctx USB context
     * @param interfaceNum Interface number to claim (e.g., INTERFACE_RECOVERY)
     */
    UsbDeviceHandle(libusb_device* device, const UsbContext& ctx,
                    int interfaceNum) noexcept;
    ~UsbDeviceHandle() noexcept;

    UsbDeviceHandle(const UsbDeviceHandle&) = delete;
    UsbDeviceHandle& operator=(const UsbDeviceHandle&) = delete;

    UsbDeviceHandle(UsbDeviceHandle&& other) noexcept :
        handle(other.handle), portPath(std::move(other.portPath)),
        claimedInterface(other.claimedInterface)
    {
        other.handle = nullptr;
        other.portPath.clear();
        other.claimedInterface = -1;
    }
    UsbDeviceHandle& operator=(UsbDeviceHandle&& other) noexcept;

    [[nodiscard]] bool isValid() const noexcept
    {
        return handle != nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return isValid();
    }
    [[nodiscard]] libusb_device_handle* get() const noexcept
    {
        return handle;
    }

    /**
     * @brief Get USB port path (e.g., "1-1.3")
     *
     * @return Port path string cached at construction time
     */
    [[nodiscard]] const std::string& getPortPath() const noexcept
    {
        return portPath;
    }

  private:
    libusb_device_handle* handle = nullptr;
    std::string portPath;
    int claimedInterface = -1;
    void cleanup() noexcept;
};

/**
 * @brief RAII wrapper for USB device reference with port path
 *
 * Automatically manages libusb_device reference counting. Device is
 * unreferenced in destructor. Move-only type to prevent double-unref.
 */
class UsbDevice
{
  public:
    UsbDevice(libusb_device* device, std::string portPath) noexcept;
    ~UsbDevice() noexcept;

    // Non-copyable (prevent double-unref)
    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;

    // Movable (transfer ownership)
    UsbDevice(UsbDevice&& other) noexcept;
    UsbDevice& operator=(UsbDevice&& other) noexcept;

    // Accessors
    [[nodiscard]] libusb_device* get() const noexcept
    {
        return device;
    }
    [[nodiscard]] const std::string& getPortPath() const noexcept
    {
        return portPath;
    }

  private:
    libusb_device* device = nullptr;
    std::string portPath;
};

/**
 * @brief Find all devices matching VID:PID=0x0955:0x7410
 *
 * Performs VID:PID matching only. Use isDeviceInRcmMode() to validate RCM mode
 * (interface 3 + endpoint 0x08 presence) if needed.
 *
 * @note Memory Management: Fully automatic via RAII. Device references are
 * managed by UsbDevice objects and automatically cleaned up when they go out of
 * scope.
 *
 * @param ctx USB context for device enumeration (must be valid)
 * @param verbose Enable diagnostic output (default: false)
 * @return Vector of RAII-managed devices on success, empty vector on failure
 */
[[nodiscard]] std::vector<UsbDevice>
    findDevicesByVidPid(const UsbContext& ctx, bool verbose = false) noexcept;

/**
 * @brief Find device by USB port path
 *
 * Searches for a device with matching VID:PID at the specified port path.
 *
 * @param ctx USB context for device enumeration (must be valid)
 * @param portPath USB port path (e.g., "1-1.3", "2-4.1")
 * @param verbose Enable diagnostic output (default: false)
 * @return Optional UsbDevice if found, std::nullopt otherwise
 */
[[nodiscard]] std::optional<UsbDevice>
    findDeviceByPortPath(const UsbContext& ctx, std::string_view portPath,
                         bool verbose = false) noexcept;

/**
 * @brief Validate device is in RCM mode (VID:PID + interface 3 + endpoint 0x08)
 *
 * Performs comprehensive validation: VID:PID match AND recovery
 * interface/endpoint presence.
 *
 * @param device USB device pointer to check (must be valid)
 * @param verbose Enable diagnostic output (default: false)
 * @return true if device has correct VID:PID AND recovery endpoints, false
 * otherwise
 */
[[nodiscard]] bool isDeviceInRcmMode(libusb_device* device,
                                     bool verbose = false) noexcept;

} // namespace usb
