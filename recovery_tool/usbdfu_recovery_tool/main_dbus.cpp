// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

/**
 * usb-dfu-recovery: systemd worker started by usb-dfu-recovery@.service with
 * the extracted recovery package directory (<UUID>) as its only argument.  It
 * resolves the preliminary bundle and the SPI image from the package
 * components (usbdfu::componentMap), reads the GPIO strap names from Entity
 * Manager (xyz.openbmc_project.Configuration.USBDFURecovery), runs the USB
 * DFU recovery sequence and reports the outcome through the Redfish message
 * registry.  The process exit code drives the systemd unit result and
 * therefore the Redfish update task.
 */

#include "config.h"

#include "dbusutils.hpp"
#include "default_config.hpp"
#include "message_registry.hpp"
#include "perform_dfu_recovery.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto usbDfuRecoveryObjInterface =
    "xyz.openbmc_project.Configuration.USBDFURecovery";
constexpr auto fallbackDeviceName = "USBDFURecovery";

constexpr auto recoveryGpioProperty = "RecoveryGpioName";
constexpr auto resetGpioProperty = "ResetGpioName";
constexpr auto spiMuxGpioProperty = "SpiMuxGpioName";
constexpr auto recoveryPolarityProperty = "RecoveryGpioPolarity";
constexpr auto resetPolarityProperty = "ResetGpioPolarity";

using nvidia::software::updater::InterfaceMap;

static auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

static std::optional<std::string>
    getStringProperty(const InterfaceMap& interfaces, const char* interface,
                      const char* property)
{
    auto ifIt = interfaces.find(interface);
    if (ifIt == interfaces.end())
    {
        return std::nullopt;
    }
    auto propIt = ifIt->second.find(property);
    if (propIt == ifIt->second.end())
    {
        return std::nullopt;
    }
    const auto* value = std::get_if<std::string>(&propIt->second);
    if (value == nullptr || value->empty())
    {
        return std::nullopt;
    }
    return *value;
}

/**
 * Populate the GPIO names in cfg from the Entity Manager object.
 * Returns the name of the first missing/empty property, or nullopt.
 */
static std::optional<std::string> readGpioConfig(const InterfaceMap& interfaces,
                                                 UsbDfuRecovery::Config& cfg)
{
    const std::pair<const char*, std::string&> fields[] = {
        {recoveryGpioProperty, cfg.recoveryGpioName},
        {resetGpioProperty, cfg.resetGpioName},
        {spiMuxGpioProperty, cfg.spiMuxGpioName},
    };
    for (const auto& [property, dest] : fields)
    {
        auto value =
            getStringProperty(interfaces, usbDfuRecoveryObjInterface, property);
        if (!value)
        {
            return std::string(property);
        }
        dest = *value;
    }
    return std::nullopt;
}

/**
 * Apply the strap polarities from Entity Manager.  Both are optional: an
 * entry that omits them keeps the compiled defaults.  A present but
 * unrecognized value is rejected rather than defaulted, because guessing
 * drives the strap the wrong way round.  Returns the offending property.
 */
static std::optional<std::string>
    readGpioPolarity(const InterfaceMap& interfaces,
                     UsbDfuRecovery::Config& cfg)
{
    const std::pair<const char*, bool&> fields[] = {
        {recoveryPolarityProperty, cfg.recoveryActiveLow},
        {resetPolarityProperty, cfg.resetActiveLow},
    };
    for (const auto& [property, dest] : fields)
    {
        auto value =
            getStringProperty(interfaces, usbDfuRecoveryObjInterface, property);
        if (!value)
        {
            continue;
        }
        auto activeLow = usbdfu::parseActiveLowPolarity(*value);
        if (!activeLow)
        {
            return std::string(property);
        }
        dest = *activeLow;
    }
    return std::nullopt;
}

static ErrorCode errorCodeOf(const nlohmann::json& out)
{
    if (out.contains("ErrorCode") && out["ErrorCode"].is_number_unsigned())
    {
        return out["ErrorCode"].get<ErrorCode>();
    }
    return static_cast<ErrorCode>(deviceRecoveryFailed);
}

} // namespace

int usbdfuServiceMain(int argc, char** argv);

int usbdfuServiceMain(int argc, char** argv)
{
    if (argc < 2)
    {
        lg2::error("Invalid number of arguments. Usage: usb-dfu-recovery "
                   "<package_dir>");
        return -1;
    }
    const std::filesystem::path packageDir = argv[1];

    auto& bus = getBus();
    nvidia::software::updater::DBUSUtils dbusUtil(bus);
    MessageRegistry messageRegistry(bus);

    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);

    std::vector<std::pair<std::string, const InterfaceMap*>> targets;
    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (!interfaces.contains(usbDfuRecoveryObjInterface))
        {
            continue;
        }
        lg2::info("Found USB DFU recovery config object: {PATH}", "PATH",
                  emObjectPath.str);
        targets.emplace_back(emObjectPath.filename(), &interfaces);
    }

    if (targets.empty())
    {
        lg2::error("No {IFACE} object found in Entity Manager", "IFACE",
                   usbDfuRecoveryObjInterface);
        messageRegistry.createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::USBDFURecovery,
            static_cast<ErrorCode>(noDevicesFound), fallbackDeviceName);
        return -1;
    }

    PackageContents package;
    nlohmann::json packageErr;
    if (!resolvePackageComponents(packageDir, package, packageErr))
    {
        lg2::error("Recovery package {DIR} is incomplete: {ERR}", "DIR",
                   packageDir.string(), "ERR",
                   packageErr.value("Error", "unknown error"));
        for (const auto& [device, interfaces] : targets)
        {
            messageRegistry.createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBDFURecovery,
                static_cast<ErrorCode>(
                    USBDFURecoveryErrorCode::PackageIncomplete),
                device);
        }
        return -1;
    }
    lg2::info("Recovery package {DIR}: {COUNT} bundle components, image "
              "{IMG}",
              "DIR", packageDir.string(), "COUNT", package.bundle.size(), "IMG",
              package.firmwareImage.string());

    int recoveryTaskState = 0;

    // Sequential on purpose: a single HMC shares the GPIO straps and USB link.
    for (const auto& [device, interfaces] : targets)
    {
        UsbDfuRecovery::Config cfg = usbdfu::makeDefaultConfig();

        if (auto missing = readGpioConfig(*interfaces, cfg))
        {
            lg2::error("Property {PROP} missing or empty for {DEVICE}", "PROP",
                       *missing, "DEVICE", device);
            messageRegistry.createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBDFURecovery,
                static_cast<ErrorCode>(
                    USBDFURecoveryErrorCode::InvalidConfiguration),
                device);
            recoveryTaskState = -1;
            continue;
        }

        if (auto bad = readGpioPolarity(*interfaces, cfg))
        {
            lg2::error("Property {PROP} is not ActiveHigh or ActiveLow for "
                       "{DEVICE}",
                       "PROP", *bad, "DEVICE", device);
            messageRegistry.createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBDFURecovery,
                static_cast<ErrorCode>(
                    USBDFURecoveryErrorCode::InvalidConfiguration),
                device);
            recoveryTaskState = -1;
            continue;
        }

        lg2::info("Starting USB DFU recovery for {DEVICE}: image {IMG}, GPIO "
                  "recovery={REC} reset={RST} spi-mux={MUX}, alt {ALT}",
                  "DEVICE", device, "IMG", package.firmwareImage.string(),
                  "REC", cfg.recoveryGpioName, "RST", cfg.resetGpioName, "MUX",
                  cfg.spiMuxGpioName, "ALT", cfg.dfuAltSetting);
        messageRegistry.createMessageRegistry(recoveryStarted, device);

        UsbDfuRecovery recovery(cfg);
        nlohmann::json out;
        if (!recovery.performFullRecovery(package.firmwareImage.string(),
                                          package.bundle, out))
        {
            const auto errorCode = errorCodeOf(out);
            lg2::error("USB DFU recovery failed for {DEVICE} (code {CODE}): "
                       "{ERR}",
                       "DEVICE", device, "CODE",
                       static_cast<unsigned>(errorCode), "ERR",
                       out.value("Error", "unknown error"));
            messageRegistry.createMessageRegistryResourceErrors(
                resourceErrorsDetected, RecoveryProtocol::USBDFURecovery,
                errorCode, device);
            recoveryTaskState = -1;
            continue;
        }

        lg2::info("USB DFU recovery completed for {DEVICE}: {RESULT}", "DEVICE",
                  device, "RESULT", out.dump());
        messageRegistry.createMessageRegistry(recoverySuccessful, device);
    }

    return recoveryTaskState;
}

int main(int argc, char** argv)
try
{
    return usbdfuServiceMain(argc, argv);
}
catch (const std::exception& e)
{
    lg2::error("usb-dfu-recovery failed: {ERR}", "ERR", e.what());
    return EXIT_FAILURE;
}
catch (...)
{
    lg2::error("usb-dfu-recovery failed with an unknown exception");
    return EXIT_FAILURE;
}
