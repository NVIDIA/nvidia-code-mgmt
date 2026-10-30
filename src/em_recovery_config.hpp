#pragma once

#include "dbusutils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{
namespace em_recovery
{

/*
 * Recovery configuration from the entity-manager platform topology.
 *
 * The platform-porting entity-manager configs describe every device once, as
 * a topology Configuration.NvDevice (Name, Kind, EID, Transport) and keep what
 * only a recovery needs in the optional NvDevice.Recovery object. This header
 * turns that into the per-type records the readers in this repository parse
 * (OCPRecovery, MCURecovery, ...), so the readers do not need to know which
 * layout entity-manager uses. Anything that a device class always has the same
 * value for lives in the tables below, not in the configuration.
 *
 * The result is added to a GetManagedObjects snapshot, see expand(). Records
 * that entity-manager publishes itself (all other platforms) are left alone.
 */

constexpr auto cfgPrefix = "xyz.openbmc_project.Configuration.";
constexpr auto nvDeviceIface = "xyz.openbmc_project.Configuration.NvDevice";
constexpr auto transportIface =
    "xyz.openbmc_project.Configuration.NvDevice.Transport";
constexpr auto deviceRecoveryIface =
    "xyz.openbmc_project.Configuration.NvDevice.Recovery";
constexpr auto recoveryMarkerIface =
    "xyz.openbmc_project.Inventory.Item.Recovery_Config";
constexpr auto usbDfuIface = "xyz.openbmc_project.Configuration.USBDFURecovery";
constexpr auto spiIface = "xyz.openbmc_project.Configuration.SPI";
constexpr auto defaultUsbBus = "1";

/** @brief Values that every device of a class shares. */
constexpr uint64_t mcuProductId = 0xCF11;
constexpr uint64_t mcuVendorId = 0x0955;
constexpr uint64_t mcuRecoveryI2cAddress = 0x10;
constexpr uint64_t gpioPollingIntervalMs = 1000;

/** @brief The mcu-recovery -t group of an MCU Kind, unless the topology says.
 */
inline const std::map<std::string, std::string, std::less<>> kindMcuTarget = {
    {"cx_sma", "CX9"},      {"bluefield_sma", "BF4"},
    {"gpu_sma", "GPU"},     {"hmc_hpm_sma", "HPM"},
    {"bmc_hpm_sma", "HPM"}, {"nvswitch_sma", "NVSSMA"},
};

inline std::optional<Value> prop(const InterfaceMap& ifaces,
                                 const std::string& iface,
                                 const std::string& key);

/**
 * @brief The records expand() derived in this process, by object path.
 *
 * They exist only in the snapshot, not on D-Bus, so a reader that follows a
 * snapshot with a property Get on entity-manager finds them here first
 * (see DBUSUtils::getPropertyImpl).
 */
inline std::map<std::string, InterfaceMap>& derivedRecords()
{
    static std::map<std::string, InterfaceMap> records;
    return records;
}

inline std::optional<Value> derivedProperty(const std::string& path,
                                            const std::string& iface,
                                            const std::string& key)
{
    auto obj = derivedRecords().find(path);
    if (obj == derivedRecords().end())
    {
        return std::nullopt;
    }
    return prop(obj->second, iface, key);
}

/** @brief "key=value;..." of a record's properties, for the derivation log. */
inline std::string describe(const PropertyMap& props)
{
    std::string out;
    for (const auto& [key, value] : props)
    {
        out += key + "=";
        std::visit(
            [&out](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    out += "\"" + v + "\"";
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    out += v ? "true" : "false";
                }
                else if constexpr (std::is_arithmetic_v<T>)
                {
                    out += std::to_string(v);
                }
                else
                {
                    out += "<bytes>";
                }
            },
            value);
        out += ";";
    }
    return out;
}

/** @brief Resolves the usb bus number of a root hub path (test hook). */
inline std::function<std::string(const std::string&)> usbBusResolver =
    [](const std::string& rootHubPath) {
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(rootHubPath, ec))
        {
            if (!entry.path().filename().string().starts_with("usb"))
            {
                continue;
            }
            std::ifstream file(entry.path() / "busnum");
            std::string bus;
            if (file >> bus)
            {
                return bus;
            }
        }
        return std::string(defaultUsbBus);
    };

/**
 * @brief Whether the interfaces of one entity-manager object are a platform
 *        topology config that expand() turns into recovery records, or that
 *        changes what it makes of the others.
 *
 * entity-manager publishes the nested objects of a record (Transport,
 * Recovery) as interfaces of their own, possibly in later signals, so a device
 * counts once its Recovery interface is there.
 */
/**
 * @brief Whether the interfaces of one object are the Transport of a device,
 *        which an SPI record that expand() withheld may have been waiting for.
 */
inline bool completesSpi(const InterfaceMap& ifaces)
{
    return ifaces.contains(transportIface);
}

inline bool isConfigObject(const InterfaceMap& ifaces)
{
    if (ifaces.contains(usbDfuIface))
    {
        return true;
    }
    return ifaces.contains(nvDeviceIface) &&
           ifaces.contains(deviceRecoveryIface);
}

inline std::optional<Value> prop(const InterfaceMap& ifaces,
                                 const std::string& iface,
                                 const std::string& key)
{
    auto it = ifaces.find(iface);
    if (it == ifaces.end())
    {
        return std::nullopt;
    }
    auto p = it->second.find(key);
    if (p == it->second.end())
    {
        return std::nullopt;
    }
    return p->second;
}

inline std::string str(const std::optional<Value>& v)
{
    if (!v)
    {
        return {};
    }
    if (const auto* s = std::get_if<std::string>(&*v))
    {
        return *s;
    }
    if (const auto* n = std::get_if<uint64_t>(&*v))
    {
        return std::to_string(*n);
    }
    if (const auto* n = std::get_if<int64_t>(&*v))
    {
        return std::to_string(*n);
    }
    return {};
}

inline bool flag(const std::optional<Value>& v, bool dflt)
{
    if (v)
    {
        if (const auto* b = std::get_if<bool>(&*v))
        {
            return *b;
        }
    }
    return dflt;
}

/** @brief entity-manager's object path segment for a record name */
inline std::string pathSegment(std::string name)
{
    for (auto& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
        {
            c = '_';
        }
    }
    return name;
}

using DeviceMap = std::map<std::string, const InterfaceMap*>;

inline void copy(PropertyMap& to, const char* toKey, const InterfaceMap& from,
                 const char* iface, const char* fromKey)
{
    if (auto v = prop(from, iface, fromKey))
    {
        to[toKey] = *v;
    }
}

/**
 * @brief Builds the record of one recovery device.
 *
 * @param[out] legacyType - the record type, e.g. "MCURecovery"
 * @return The record properties, or nothing for an unknown method
 */
inline std::optional<PropertyMap>
    makeRecord(const std::string& method, const std::string& name,
               const std::string& kind, const InterfaceMap& dev,
               const DeviceMap& devices, std::string& legacyType)
{
    const auto eid = prop(dev, nvDeviceIface, "EID");
    const std::string port = str(prop(dev, transportIface, "Port"));
    const std::string hub = str(prop(dev, transportIface, "RootHubPath"));
    const std::string swName =
        str(prop(dev, deviceRecoveryIface, "SoftwareName"));
    // A device names what it exposes in its Recovery block; without a name
    // the object is named after the device.
    const auto named = [&](const char* key, const std::string& dflt) {
        const std::string v = str(prop(dev, deviceRecoveryIface, key));
        return v.empty() ? dflt : v;
    };
    PropertyMap r;
    r["Name"] = swName.empty() ? "FW_" + name : swName;

    if (method == "OCP")
    {
        legacyType = "OCPRecovery";
        r["ChassisName"] = named("ChassisName", "IRoT_" + name);
        r["ForceRecoveryChassisObject"] = name;
        r["hasChassisPowerSource"] = true;
        if (flag(prop(dev, deviceRecoveryIface, "OcpInfoRom"), true))
        {
            r["InfoRomName"] = named("InfoRomName", "InfoROM_" + name);
        }
        if (auto bp = prop(dev, transportIface, "BridgePort"))
        {
            std::string bus = defaultUsbBus;
            auto bridge =
                devices.find(str(prop(dev, transportIface, "Bridge")));
            if (bridge != devices.end())
            {
                bus = usbBusResolver(
                    str(prop(*bridge->second, transportIface, "RootHubPath")));
            }
            r["USBPort"] = bus + "-" + str(bp);
        }
        copy(r, "I2CAddress", dev, deviceRecoveryIface,
             "OcpRecoveryI2cAddress");
        copy(r, "I2CBus", dev, deviceRecoveryIface, "OcpRecoveryI2cBus");
        if (flag(prop(dev, deviceRecoveryIface, "OcpSmaEid"), true))
        {
            copy(r, "SMAEID", dev, transportIface, "BridgeEID");
        }
    }
    else if (method == "USBRCM")
    {
        const std::string inst = str(prop(dev, nvDeviceIface, "Instance"));
        legacyType = "USBRCMRecovery";
        r["FMCComponentName"] = named("FMCComponentName", "SBIOS_FMC_" + inst);
        r["FWSComponentName"] = named("FWSComponentName", "SBIOS_FW_" + inst);
        r["USBPort"] = usbBusResolver(hub) + "-" + port;
        r["hasChassisPowerSource"] = true;
    }
    else if (method == "ConnectX" || method == "NVSwitch" ||
             method == "NVLinkManagementNIC")
    {
        legacyType =
            method == "ConnectX" ? "ConnectXRecovery" : method + "Recovery";
        // The force action sits on the NIC's own chassis unless the device
        // asks for its IRoT chassis.
        const bool onIRoT =
            method == "ConnectX" &&
            flag(prop(dev, deviceRecoveryIface, "ForceRecoveryOnIRoT"), false);
        const std::string chassis = named("ChassisName", "IRoT_" + name);
        r["ChassisName"] = chassis;
        r["ForceRecoveryChassisObject"] = onIRoT ? chassis : name;
        r["hasChassisPowerSource"] = true;
        copy(r, "EID", dev, nvDeviceIface, "EID");
        copy(r, "SMAEID", dev, transportIface, "BridgeEID");
        copy(r, "I2CAddress", dev, deviceRecoveryIface, "I2CAddress");
        copy(r, "I2CBus", dev, deviceRecoveryIface, "I2CBus");
        copy(r, "ResetGPIO", dev, deviceRecoveryIface, "ResetGpio");
        copy(r, "FlashNotPresentGPIO", dev, deviceRecoveryIface,
             "FlashNotPresentGpio");
    }
    else if (method == "GlacierCrisis")
    {
        legacyType = "GlacierCrisisRecovery";
        r["APBootStatusType"] = std::string("QueryBootStatus");
        r["ChassisName"] = name;
        r["hasChassisPowerSource"] = false;
        r["isRecoverable"] = false;
    }
    else if (method == "MCU")
    {
        legacyType = "MCURecovery";
        r["ForceRecoveryChassisObject"] = name;
        // Read by the MCU tool's --chassis filter.
        r["ChassisName"] = name;
        r["hasChassisPowerSource"] = false;
        r["RecoveryGpioName"] =
            str(prop(dev, deviceRecoveryIface, "RecoveryGpio"));
        r["ResetGpioName"] = str(prop(dev, deviceRecoveryIface, "ResetGpio"));
        copy(r, "ResetGpioPolarity", dev, deviceRecoveryIface,
             "ResetGpioPolarity");
        std::string target = str(prop(dev, deviceRecoveryIface, "Target"));
        if (target.empty())
        {
            if (auto t = kindMcuTarget.find(kind); t != kindMcuTarget.end())
            {
                target = t->second;
            }
        }
        r["Target"] = target;
        if (str(prop(dev, transportIface, "Medium")) == "i2c")
        {
            r["Interface"] = std::string("I2C");
            r["RecoveryI2CAddress"] = mcuRecoveryI2cAddress;
            copy(r, "I2CBus", dev, transportIface, "Bus");
            copy(r, "NormalI2CAddress", dev, transportIface, "Address");
        }
        else
        {
            r["Interface"] = std::string("USB");
            r["ProductId"] = mcuProductId;
            r["VendorId"] = mcuVendorId;
            r["USBPort"] = usbBusResolver(hub) + "-" + port;
        }
    }
    else if (method == "GPIOERoT")
    {
        legacyType = "GPIOERoTRecovery";
        r["HideWhenHealthy"] = true;
        r["MonitorMode"] = std::string("Polling");
        r["PollingIntervalMs"] = gpioPollingIntervalMs;
        r["HiddenByFPGA"] = false;
        r["hasChassisPowerSource"] = false;
        copy(r, "GPIO", dev, deviceRecoveryIface, "FatalErrorGpio");
        copy(r, "Polarity", dev, deviceRecoveryIface, "FatalErrorPolarity");
        copy(r, "APName", dev, deviceRecoveryIface, "ApName");
        copy(r, "Target", dev, deviceRecoveryIface, "Target");
        copy(r, "I2CAddress", dev, deviceRecoveryIface, "I2CAddress");
        copy(r, "I2CBus", dev, deviceRecoveryIface, "I2CBus");
    }
    else
    {
        lg2::error("Unknown RecoveryType {METHOD} for {NAME}", "METHOD", method,
                   "NAME", name);
        return std::nullopt;
    }

    if (eid && method != "ConnectX" && method != "NVSwitch" &&
        method != "NVLinkManagementNIC")
    {
        r["MctpEID"] = *eid;
    }
    r["Type"] = legacyType;
    return r;
}

/** The strap lines a CPU board drives, in the order of its StrapPins. */
constexpr std::array<const char*, 6> cpuStrapKeys = {
    "ForcedRecoveryGpio", "BootDevSel0Gpio",   "BootDevSel1Gpio",
    "BootDevSel2Gpio",    "RecoveryType0Gpio", "RecoveryType1Gpio"};
/** Strap levels that put a CPU into USB RCM recovery (forced recovery low,
 *  boot device 0b000, recovery type 0b10) and its normal-boot levels. */
constexpr std::array<double, 6> cpuStrapActive = {0, 0, 0, 0, 0, 1};
constexpr std::array<double, 6> cpuStrapDefault = {1, 0, 0, 0, 0, 1};

/** The lines of the boards that one chassis resets and straps together. */
struct CpuBoards
{
    std::vector<std::string> resets;
    std::vector<std::string> straps;
};

/**
 * One USBRCMForceRecovery record per chassis that carries the Redfish action,
 * in the shape a develop record has: every board's reset line in ResetPins and
 * its six strap lines in StrapPins. CPUs of one board name the same lines, so
 * a board is listed once.
 */
inline void deriveCpuForceRecovery(const ObjectValueTree& objects,
                                   ObjectValueTree& added)
{
    std::map<std::string, CpuBoards> chassisBoards;
    std::map<std::string, std::string> chassisDir;
    for (const auto& [path, ifaces] : objects)
    {
        const std::string chassis =
            str(prop(ifaces, deviceRecoveryIface, "ActionRedfishChassis"));
        const std::string reset =
            str(prop(ifaces, deviceRecoveryIface, "SystemResetGpio"));
        if (chassis.empty() || reset.empty() ||
            str(prop(ifaces, nvDeviceIface, "Kind")) != "cpu")
        {
            continue;
        }
        std::vector<std::string> straps;
        for (const char* key : cpuStrapKeys)
        {
            straps.push_back(str(prop(ifaces, deviceRecoveryIface, key)));
        }
        if (std::ranges::find(straps, std::string()) != straps.end())
        {
            continue;
        }
        auto& boards = chassisBoards[chassis];
        if (std::ranges::find(boards.resets, reset) != boards.resets.end())
        {
            continue;
        }
        boards.resets.push_back(reset);
        boards.straps.insert(boards.straps.end(), straps.begin(), straps.end());
        chassisDir.emplace(chassis, path.parent_path().str);
    }

    for (const auto& [chassis, boards] : chassisBoards)
    {
        std::vector<double> active;
        std::vector<double> defaults;
        for (size_t i = 0; i < boards.resets.size(); ++i)
        {
            active.insert(active.end(), cpuStrapActive.begin(),
                          cpuStrapActive.end());
            defaults.insert(defaults.end(), cpuStrapDefault.begin(),
                            cpuStrapDefault.end());
        }
        const std::string name = chassis + "_CPU_Recovery";
        PropertyMap record{{"Name", name},
                           {"Type", std::string("USBRCMForceRecovery")},
                           {"ForceRecoveryChassisObject", chassis},
                           {"ResetPins", boards.resets},
                           {"StrapPins", boards.straps},
                           {"StrapActiveValues", active},
                           {"StrapDefaultValues", defaults}};
        added[sdbusplus::object_path(chassisDir.at(chassis) + "/" +
                                     pathSegment(name))] = {
            {std::string(cfgPrefix) + "USBRCMForceRecovery", std::move(record)},
            {recoveryMarkerIface, {}}};
    }
}

/**
 * @brief Adds the records a platform topology implies to a snapshot of
 *        entity-manager's objects.
 *
 * Idempotent, and a no-op for a snapshot without a recovery device.
 */
inline void expand(ObjectValueTree& objects)
{
    DeviceMap devices;
    bool haveTopology = false;
    for (const auto& [path, ifaces] : objects)
    {
        if (ifaces.contains(nvDeviceIface))
        {
            devices[str(prop(ifaces, nvDeviceIface, "Name"))] = &ifaces;
            haveTopology = haveTopology || ifaces.contains(deviceRecoveryIface);
        }
    }
    // SPI records name their device (Device, MuxGpio); the readers want the
    // USB port, programmer and inventory path. They create their object from
    // the first snapshot that has the record and never revisit it, so a record
    // whose device is not published yet is withheld, not half translated. The
    // device or its transport arriving is a later interfacesAdded.
    for (auto& [path, ifaces] : objects)
    {
        auto it = ifaces.find(spiIface);
        if (it == ifaces.end() || it->second.contains("USBPort") ||
            !it->second.contains("Device"))
        {
            continue;
        }
        auto dev = devices.find(str(prop(ifaces, spiIface, "Device")));
        if (dev == devices.end() || !dev->second->contains(transportIface))
        {
            ifaces.erase(std::string(spiIface) + ".Gpio0");
            ifaces.erase(it);
            continue;
        }
        it->second["USBPort"] =
            usbBusResolver(
                str(prop(*dev->second, transportIface, "RootHubPath"))) +
            "-" + str(prop(*dev->second, transportIface, "Port"));
        it->second.try_emplace("Programmer", std::string("nv_sma_spi"));
        it->second.try_emplace(
            "InventoryObjPath",
            std::string("/xyz/openbmc_project/inventory/system"));
        if (auto gpio = prop(ifaces, spiIface, "MuxGpio"))
        {
            ifaces[std::string(spiIface) + ".Gpio0"] = {
                {"GpioLine", *gpio},
                {"ActiveValue", std::string("high")},
                {"DeactiveValue", std::string("low")}};
        }
    }

    if (!haveTopology)
    {
        return;
    }

    ObjectValueTree added;
    for (auto& [path, ifaces] : objects)
    {
        if (auto it = ifaces.find(usbDfuIface); it != ifaces.end())
        {
            it->second.try_emplace("RecoveryGpioPolarity",
                                   std::string("ActiveHigh"));
            it->second.try_emplace("ResetGpioPolarity",
                                   std::string("ActiveLow"));
            ifaces.try_emplace(recoveryMarkerIface);
        }
        if (!ifaces.contains(nvDeviceIface))
        {
            continue;
        }
        const std::string name = str(prop(ifaces, nvDeviceIface, "Name"));
        const std::string kind = str(prop(ifaces, nvDeviceIface, "Kind"));
        // The device says how it is recovered; without a RecoveryType it is
        // not a recovery device.
        const std::string method =
            str(prop(ifaces, deviceRecoveryIface, "RecoveryType"));
        if (method.empty())
        {
            continue;
        }
        // A record that reads the transport waits until its interface is
        // published, so a snapshot never yields a half-described device.
        if (!ifaces.contains(transportIface) &&
            (method == "MCU" || method == "OCP" || method == "USBRCM" ||
             method == "ConnectX" || method == "NVSwitch" ||
             method == "NVLinkManagementNIC"))
        {
            continue;
        }
        std::string legacyType;
        auto record =
            makeRecord(method, name, kind, ifaces, devices, legacyType);
        if (!record)
        {
            continue;
        }
        const std::string recordPath =
            path.parent_path().str + "/" +
            pathSegment(std::get<std::string>((*record)["Name"]));
        added[sdbusplus::object_path(recordPath)] = {
            {std::string(cfgPrefix) + legacyType, std::move(*record)},
            {recoveryMarkerIface, {}}};
    }

    deriveCpuForceRecovery(objects, added);

    for (const auto& [path, ifaces] : added)
    {
        // Once per record: what was derived, to compare with a develop image.
        if (!derivedRecords().contains(path.str))
        {
            for (const auto& [iface, props] : ifaces)
            {
                if (iface.starts_with(cfgPrefix))
                {
                    lg2::info("em_recovery derived {PATH} {IFACE} {PROPS}",
                              "PATH", path.str, "IFACE",
                              iface.substr(std::string_view(cfgPrefix).size()),
                              "PROPS", describe(props));
                }
            }
        }
        derivedRecords()[path.str] = ifaces;
    }
    objects.merge(added);
}

} // namespace em_recovery
} // namespace updater
} // namespace software
} // namespace nvidia
