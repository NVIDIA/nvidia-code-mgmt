#pragma once

#include "dbusutils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

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
/** entity-manager only publishes the Name, Probe and Type of the Platform
 *  record itself, so the values NCM needs (name prefix, top chassis, policy
 *  flags) are carried by one RecoveryPolicy record in PlatformExposes. */
constexpr auto platformIface =
    "xyz.openbmc_project.Configuration.RecoveryPolicy";
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

/**
 * @brief Recovery method of a device class, by NvDevice Kind.
 *
 * NvDevice.Recovery.Method overrides it. A Kind in this table with a Recovery
 * object is a recovery device; the Kinds in implicitKinds are recovery
 * devices without one (nothing in their record varies).
 */
inline const std::map<std::string, std::string, std::less<>> kindMethod = {
    {"cx_sma", "MCU"},         {"bluefield_sma", "MCU"},
    {"gpu_sma", "MCU"},        {"hmc_hpm_sma", "MCU"},
    {"bmc_hpm_sma", "MCU"},    {"nvswitch_sma", "MCU"},
    {"cx9_io", "ConnectX"},    {"gpu", "OCP"},
    {"nvswitch", "NVSwitch"},  {"nvlink_mgmt_nic", "NVLinkManagementNIC"},
    {"erot", "GlacierCrisis"}, {"cpu_erot", "GlacierCrisis"},
    {"cpu", "USBRCM"},
};

inline const std::set<std::string, std::less<>> implicitKinds = {
    "erot", "cpu_erot", "cpu"};

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
 * counts once its Recovery interface is there (or at once for a Kind that has
 * none, see implicitKinds).
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
    if (ifaces.contains(platformIface) || ifaces.contains(usbDfuIface))
    {
        return true;
    }
    auto dev = ifaces.find(nvDeviceIface);
    if (dev == ifaces.end())
    {
        return false;
    }
    if (ifaces.contains(deviceRecoveryIface))
    {
        return true;
    }
    auto kind = dev->second.find("Kind");
    if (kind == dev->second.end())
    {
        return false;
    }
    const auto* name = std::get_if<std::string>(&kind->second);
    return name != nullptr && implicitKinds.contains(*name);
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

struct Platform
{
    std::string path;
    std::string prefix;
    std::string topChassis;
    std::string cpuGpioStraps;
    std::string cpuActionChassis;
    bool connectXForceOnIRoT = true;
    bool ocpInfoRom = true;
    bool ocpSmaEid = true;
    bool mcuChassisName = true;
};

using DeviceMap = std::map<std::string, const InterfaceMap*>;

/** @brief Device name without the platform name prefix ("HGX_GPU_0" -> "GPU_0")
 */
inline std::string baseName(const Platform& platform, const std::string& name)
{
    const auto& p = platform.prefix;
    return (!p.empty() && name.starts_with(p)) ? name.substr(p.size()) : name;
}

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
               const DeviceMap& devices, const Platform& platform,
               std::string& legacyType)
{
    const std::string& p = platform.prefix;
    const std::string base = baseName(platform, name);
    const auto eid = prop(dev, nvDeviceIface, "EID");
    const std::string port = str(prop(dev, transportIface, "Port"));
    const std::string hub = str(prop(dev, transportIface, "RootHubPath"));
    const std::string swName =
        str(prop(dev, deviceRecoveryIface, "SoftwareName"));
    PropertyMap r;
    r["Name"] = swName.empty() ? p + "FW_" + base : swName;

    if (method == "OCP")
    {
        legacyType = "OCPRecovery";
        r["ChassisName"] = p + "IRoT_" + base;
        r["ForceRecoveryChassisObject"] = name;
        r["hasChassisPowerSource"] = true;
        if (platform.ocpInfoRom)
        {
            r["InfoRomName"] = p + "InfoROM_" + base;
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
        if (platform.ocpSmaEid)
        {
            copy(r, "SMAEID", dev, transportIface, "BridgeEID");
        }
    }
    else if (method == "USBRCM")
    {
        const std::string inst = str(prop(dev, nvDeviceIface, "Instance"));
        legacyType = "USBRCMRecovery";
        r["FMCComponentName"] = p + "SBIOS_FMC_" + inst;
        r["FWSComponentName"] = p + "SBIOS_FW_" + inst;
        r["USBPort"] = usbBusResolver(hub) + "-" + port;
        r["hasChassisPowerSource"] = true;
    }
    else if (method == "ConnectX" || method == "NVSwitch" ||
             method == "NVLinkManagementNIC")
    {
        legacyType =
            method == "ConnectX" ? "ConnectXRecovery" : method + "Recovery";
        const bool onIRoT =
            method == "ConnectX" && platform.connectXForceOnIRoT;
        r["ChassisName"] = p + "IRoT_" + base;
        r["ForceRecoveryChassisObject"] = onIRoT ? p + "IRoT_" + base : name;
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
        if (platform.mcuChassisName)
        {
            // Read by the MCU tool's --chassis filter; the vr-nvl HMC
            // platforms never had it.
            r["ChassisName"] = name;
        }
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
        lg2::error("Unknown recovery method {METHOD} for {NAME}", "METHOD",
                   method, "NAME", name);
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

/**
 * @brief Adds the records a platform topology implies to a snapshot of
 *        entity-manager's objects.
 *
 * Idempotent, and a no-op for a snapshot without a platform topology.
 */
inline void expand(ObjectValueTree& objects)
{
    Platform platform;
    DeviceMap devices;
    bool haveTopology = false;
    for (const auto& [path, ifaces] : objects)
    {
        if (ifaces.contains(platformIface))
        {
            platform.path = path.str.substr(0, path.str.rfind('/'));
            platform.prefix = str(prop(ifaces, platformIface, "NamePrefix"));
            platform.topChassis =
                str(prop(ifaces, platformIface, "TopChassis"));
            platform.cpuGpioStraps =
                str(prop(ifaces, platformIface, "CpuRecoveryGPIOStraps"));
            platform.cpuActionChassis = str(
                prop(ifaces, platformIface, "CpuRecoveryActionRedfishChassis"));
            platform.connectXForceOnIRoT =
                flag(prop(ifaces, platformIface, "ConnectXForceRecoveryOnIRoT"),
                     true);
            platform.ocpInfoRom =
                flag(prop(ifaces, platformIface, "OcpInfoRom"), true);
            platform.ocpSmaEid =
                flag(prop(ifaces, platformIface, "OcpSmaEid"), true);
            platform.mcuChassisName =
                flag(prop(ifaces, platformIface, "McuChassisName"), true);
            haveTopology = true;
        }
        if (ifaces.contains(nvDeviceIface))
        {
            devices[str(prop(ifaces, nvDeviceIface, "Name"))] = &ifaces;
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
        std::string method = str(prop(ifaces, deviceRecoveryIface, "Method"));
        if (method.empty())
        {
            if (!ifaces.contains(deviceRecoveryIface) &&
                !implicitKinds.contains(kind))
            {
                continue;
            }
            auto m = kindMethod.find(kind);
            if (m == kindMethod.end())
            {
                continue;
            }
            method = m->second;
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
        auto record = makeRecord(method, name, kind, ifaces, devices, platform,
                                 legacyType);
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

    if (!platform.cpuGpioStraps.empty() && !platform.cpuActionChassis.empty())
    {
        const std::string name =
            (platform.topChassis.empty() ? platform.cpuActionChassis
                                         : platform.topChassis) +
            "_CPU_Recovery";
        added[sdbusplus::object_path(platform.path + "/" + pathSegment(name))] =
            {{std::string(cfgPrefix) + "USBRCMForceRecovery",
              {{"Name", name},
               {"Type", std::string("USBRCMForceRecovery")},
               {"ConfigType", platform.cpuGpioStraps},
               {"ForceRecoveryChassisObject", platform.cpuActionChassis}}},
             {recoveryMarkerIface, {}}};
    }

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
