// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "usbdfu_status.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace usbdfu
{

namespace
{

static std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

} // namespace

bool isRecoveryAlt(std::string_view name) noexcept
{
    return name.starts_with("recovery");
}

std::optional<UsbId> UsbId::parse(std::string_view text)
{
    static const std::regex re("^([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})$");
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_match(text.begin(), text.end(), m, re))
    {
        return std::nullopt;
    }
    return UsbId{lower(m[1].str()), lower(m[2].str())};
}

const char* toString(RecoveryState state) noexcept
{
    switch (state)
    {
        case RecoveryState::InRecovery:
            return "In Recovery";
        case RecoveryState::NotInRecovery:
            return "Not in Recovery";
        case RecoveryState::Unknown:
        default:
            return "Unknown";
    }
}

const char* toString(RecoveryStage stage) noexcept
{
    switch (stage)
    {
        case RecoveryStage::BootRom:
            return "BootROM";
        case RecoveryStage::RecoveryUBoot:
            return "RecoveryUBoot";
        case RecoveryStage::None:
        default:
            return "None";
    }
}

nlohmann::json RecoveryStatus::toJson() const
{
    nlohmann::json j;
    j["Recovery Status"] = toString(state);
    j["Recovery Stage"] = toString(stage);
    j["USB Device Present"] = usbDevicePresent;
    j["USB Port Path"] = usbPortPath;
    j["USB ID"] = usbId;
    j["DFU Alt Settings"] = dfuAltSettings;
    if (!error.empty())
    {
        j["Error"] = error;
    }
    return j;
}

std::vector<DfuAlt> parseDfuUtilList(std::string_view output)
{
    // e.g.  Found DFU: [2245:2700] ver=0100, devnum=11, cfg=1, intf=0,
    //         path="1-1", alt=0, name="recovery_cs0", serial="UNKNOWN"
    static const std::regex idRe("\\[([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})\\]");
    static const std::regex pathRe("path=\"([^\"]*)\"");
    static const std::regex nameRe("name=\"([^\"]*)\"");

    std::vector<DfuAlt> alts;
    std::istringstream in{std::string(output)};
    std::string line;
    while (std::getline(in, line))
    {
        if (line.find("Found DFU:") == std::string::npos)
        {
            continue;
        }
        DfuAlt alt;
        std::smatch m;
        if (std::regex_search(line, m, idRe))
        {
            alt.usbId = lower(m[1].str()) + ":" + lower(m[2].str());
        }
        if (std::regex_search(line, m, pathRe))
        {
            alt.path = m[1].str();
        }
        if (std::regex_search(line, m, nameRe))
        {
            alt.name = m[1].str();
        }
        alts.push_back(std::move(alt));
    }
    return alts;
}

RecoveryStatus
    deriveRecoveryStatus(const std::optional<usb_sysfs::Device>& bootRom,
                         const std::vector<DfuAlt>& dfuAlts,
                         const std::optional<usb_sysfs::Device>& functional,
                         bool functionalConfigured, std::string_view portPath)
{
    RecoveryStatus status;
    for (const auto& alt : dfuAlts)
    {
        status.dfuAltSettings.push_back(alt.name);
    }
    // The device identity is taken from the recovery_* entry itself, not
    // from whichever alt dfu-util happened to list first (recovery U-Boot
    // also exposes fw_logs_cs0/cs1).
    // Only a recovery_* target on the probed port counts: `dfu-util -l`
    // without -p lists every DFU device on the BMC, and another HMC's
    // recovery U-Boot must not make this one look recovered.
    const auto recoveryAlt =
        std::find_if(dfuAlts.begin(), dfuAlts.end(), [&](const DfuAlt& a) {
            return isRecoveryAlt(a.name) &&
                   (portPath.empty() || a.path == portPath);
        });
    const bool recoveryUBoot = recoveryAlt != dfuAlts.end();

    if (bootRom || recoveryUBoot)
    {
        status.state = RecoveryState::InRecovery;
        status.stage = recoveryUBoot ? RecoveryStage::RecoveryUBoot
                                     : RecoveryStage::BootRom;
        status.usbDevicePresent = true;
        if (bootRom)
        {
            status.usbPortPath = bootRom->portPath;
            status.usbId = bootRom->vid + ":" + bootRom->pid;
        }
        else
        {
            status.usbPortPath = recoveryAlt->path;
            status.usbId = recoveryAlt->usbId;
        }
        return status;
    }

    if (functional)
    {
        status.state = RecoveryState::NotInRecovery;
        status.usbDevicePresent = true;
        status.usbPortPath = functional->portPath;
        status.usbId = functional->vid + ":" + functional->pid;
        return status;
    }

    status.state = RecoveryState::Unknown;
    status.error =
        functionalConfigured
            ? "HMC not found on USB: neither the DFU device nor the "
              "functional identity is present"
            : "HMC not in DFU mode and no functional USB identity is "
              "configured; health cannot be determined";
    return status;
}

RecoveryStatus getRecoveryStatus(const StatusConfig& cfg,
                                 const CaptureFn& capture)
{
    const auto bootRom =
        usb_sysfs::findDevice(cfg.usbSysfsRoot, cfg.bootRomId.vid,
                              cfg.bootRomId.pid, cfg.usbPortFilter);

    std::optional<usb_sysfs::Device> functional;
    if (cfg.functionalId)
    {
        functional =
            usb_sysfs::findDevice(cfg.usbSysfsRoot, cfg.functionalId->vid,
                                  cfg.functionalId->pid, cfg.usbPortFilter);
    }

    std::vector<DfuAlt> alts;
    std::string listError;
    if (capture)
    {
        std::vector<std::string> argv = {cfg.dfuUtilPath, "-l"};
        if (!cfg.usbPortFilter.empty())
        {
            argv.push_back("-p");
            argv.push_back(cfg.usbPortFilter);
        }
        if (auto out = capture(argv, cfg.dfuListTimeoutSecs))
        {
            alts = parseDfuUtilList(*out);
        }
        else
        {
            listError = "dfu-util -l could not be executed";
            lg2::warning("{ERR}", "ERR", listError);
        }
    }

    // Correlate the alt listing with the device we probed: the configured
    // port if any, else the port the BootROM / recovery gadget was seen on.
    std::string expectedPort = cfg.usbPortFilter;
    if (expectedPort.empty() && bootRom)
    {
        expectedPort = bootRom->portPath;
    }
    auto status = deriveRecoveryStatus(
        bootRom, alts, functional, cfg.functionalId.has_value(), expectedPort);
    if (!listError.empty() && status.error.empty())
    {
        status.error = listError;
    }
    lg2::info("HMC recovery status: {STATE} (stage {STAGE}, usb {ID} on "
              "{PORT})",
              "STATE", toString(status.state), "STAGE", toString(status.stage),
              "ID", status.usbId, "PORT", status.usbPortPath);
    return status;
}

} // namespace usbdfu
