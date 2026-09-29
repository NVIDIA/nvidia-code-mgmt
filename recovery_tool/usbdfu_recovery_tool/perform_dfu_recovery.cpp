// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "perform_dfu_recovery.hpp"

#include "process_utils.hpp"
#include "usb_sysfs.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr auto gpioConsumer = "usbdfu-recovery";

inline std::uint8_t code(USBDFURecoveryErrorCode c)
{
    return static_cast<std::uint8_t>(c);
}

} // namespace

// static
bool UsbDfuRecovery::setFailure(nlohmann::json& out, const std::string& error,
                                USBDFURecoveryErrorCode errorCode)
{
    out["Status"] = "Failed";
    out["Error"] = error;
    out["ErrorCode"] = code(errorCode);
    return false;
}

// static
bool UsbDfuRecovery::setFailure(nlohmann::json& out, const std::string& error)
{
    out["Status"] = "Failed";
    out["Error"] = error;
    return false;
}

// static
bool UsbDfuRecovery::failStep(nlohmann::json& out, const char* step,
                              const nlohmann::json& stepResult,
                              const std::string& fallback,
                              USBDFURecoveryErrorCode fallbackCode)
{
    out["Steps"][step] = "Failed";
    return setFailure(out, stepResult.value("Error", fallback),
                      static_cast<USBDFURecoveryErrorCode>(
                          stepResult.value("ErrorCode", code(fallbackCode))));
}

// static
bool UsbDfuRecovery::pollUntil(int timeoutMs, int intervalMs,
                               const std::function<bool()>& ready)
{
    const int timeout = std::max(0, timeoutMs);
    const int interval = std::max(1, intervalMs);
    for (int elapsed = 0;;)
    {
        if (ready())
        {
            return true;
        }
        if (elapsed >= timeout)
        {
            return false;
        }
        const int nap = std::min(interval, timeout - elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(nap));
        elapsed += nap;
    }
}

UsbDfuRecovery::UsbDfuRecovery(const Config& config) : config_(config)
{}

// static
gpiod::line UsbDfuRecovery::requestOutputLine(const std::string& pinName,
                                              int value,
                                              std::string& errorMsg) noexcept
{
    gpiod::line line;
    try
    {
        // find_line() opens every gpiochip and may throw (e.g. EACCES).
        line = gpiod::find_line(pinName);
    }
    catch (const std::exception& e)
    {
        errorMsg = "GPIO lookup failed for " + pinName + ": " + e.what();
        return {};
    }
    if (!line)
    {
        errorMsg = "GPIO line not found: " + pinName;
        return {};
    }
    try
    {
        line.request({gpioConsumer, gpiod::line_request::DIRECTION_OUTPUT, 0});
        line.set_value(value);
    }
    catch (const std::exception& e)
    {
        errorMsg = "Failed to drive GPIO " + pinName + "=" +
                   std::to_string(value) + ": " + e.what();
        return {};
    }
    return line;
}

bool UsbDfuRecovery::pulseReset() noexcept
{
    lg2::debug("Pulsing reset GPIO {GPIO}: low -> {PULSE}s -> high", "GPIO",
               config_.resetGpioName, "PULSE", usbdfu::timing::resetPulseSecs);
    std::string err;
    gpiod::line line = requestOutputLine(config_.resetGpioName, 0, err);
    if (!line)
    {
        lg2::error("Pulse reset failed: {ERR}", "ERR", err);
        return false;
    }
    std::this_thread::sleep_for(
        std::chrono::seconds(usbdfu::timing::resetPulseSecs));
    try
    {
        line.set_value(1);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to release reset GPIO: {ERR}", "ERR", e.what());
        return false;
    }
    return true;
}

bool UsbDfuRecovery::runDfuUtil(const std::vector<std::string>& dfuUtilArgs,
                                int timeoutSecs, nlohmann::json& out) noexcept
{
    std::vector<std::string> argv = {config_.dfuUtilPath};
    argv.insert(argv.end(), dfuUtilArgs.begin(), dfuUtilArgs.end());

    std::string cmdLog = config_.dfuUtilPath;
    for (const auto& a : dfuUtilArgs)
    {
        cmdLog += " " + a;
    }
    lg2::info("Running: {CMD} (timeout {T}s)", "CMD", cmdLog, "T", timeoutSecs);
    lastCommand_ = cmdLog;

    // dfu-util puts every useful diagnostic ("No DFU capable USB device",
    // "Cannot set alternate interface: LIBUSB_ERROR_OTHER") on stderr, so
    // capture it and keep the tail for the journal and the JSON result.
    int exitCode = -1;
    auto output = usbdfu::process::capture(argv, timeoutSecs, &exitCode);
    if (output && exitCode == 0)
    {
        return true;
    }

    std::string tail;
    if (output)
    {
        std::vector<std::string> lines;
        std::istringstream in(*output);
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty())
            {
                lines.push_back(line);
            }
        }
        constexpr size_t maxLines = 5;
        size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
        for (size_t i = first; i < lines.size(); ++i)
        {
            tail += (tail.empty() ? "" : " | ") + lines[i];
        }
    }
    lg2::error("dfu-util failed (exit {CODE}): {CMD}; output: {OUT}", "CODE",
               exitCode, "CMD", cmdLog, "OUT",
               tail.empty() ? std::string("<none>") : tail);
    out["Error"] = "dfu-util failed: " + cmdLog;
    out["DfuUtilExitCode"] = exitCode;
    out["DfuUtilOutput"] = tail;
    return false;
}

UsbDfuRecovery::DfuTransferParams
    UsbDfuRecovery::transferParams(DfuTransfer transfer) const
{
    switch (transfer)
    {
        case DfuTransfer::FirmwareImage:
        {
            std::vector<std::string> args = {"-a", config_.dfuAltSetting};
            return {"Firmware image", std::move(args),
                    config_.flashTimeoutSecs};
        }
        case DfuTransfer::BundleStage:
        default:
            return {"Bundle binary", {}, config_.bundleStepTimeoutSecs};
    }
}

bool UsbDfuRecovery::runDfuDownload(DfuTransfer transfer,
                                    const std::string& filePath,
                                    nlohmann::json& out)
{
    const DfuTransferParams params = transferParams(transfer);

    // File existence is a precondition established by preflightChecks().
    std::vector<std::string> args(params.dfuUtilArgs);
    args.push_back("-D");
    args.push_back(filePath);

    nlohmann::json dfuOut;
    if (!runDfuUtil(args, params.timeoutSecs, dfuOut))
    {
        out["Status"] = "Failed";
        out["Error"] = dfuOut.value("Error", "dfu-util download failed for " +
                                                 params.label);
        if (dfuOut.contains("DfuUtilOutput"))
        {
            out["DfuUtilOutput"] = dfuOut["DfuUtilOutput"];
            out["DfuUtilExitCode"] = dfuOut["DfuUtilExitCode"];
        }
        return false;
    }
    return true;
}

bool UsbDfuRecovery::setRecoveryStrap(int recoveryValue, nlohmann::json& out)
{
    // Release any lines held by a previous call before re-acquiring.
    session_.release();

    std::string err;
    session_.spiMuxLine = requestOutputLine(config_.spiMuxGpioName, 0, err);
    if (!session_.spiMuxLine)
    {
        return setFailure(out, "Failed to set SPI MUX GPIO: " + err);
    }

    session_.recoveryLine =
        requestOutputLine(config_.recoveryGpioName, recoveryValue, err);
    if (!session_.recoveryLine)
    {
        session_.release();
        return setFailure(out,
                          std::string(recoveryValue ? "Failed to assert"
                                                    : "Failed to deassert") +
                              " recovery GPIO: " + err);
    }

    std::this_thread::sleep_for(
        std::chrono::seconds(usbdfu::timing::strapSettleSecs));

    if (!pulseReset())
    {
        session_.release();
        return setFailure(out, "Failed to pulse reset GPIO: " +
                                   config_.resetGpioName);
    }

    out["Status"] = "Successful";
    return true;
}

bool UsbDfuRecovery::waitForDfuEnumeration(nlohmann::json& out)
{
    lg2::info("Waiting up to {T}s for HMC to enumerate as USB DFU device "
              "{VID}:{PID}",
              "T", config_.dfuEnumerationTimeoutSecs, "VID",
              config_.dfuVendorId, "PID", config_.dfuProductId);

    const bool enumerated =
        pollUntil(config_.dfuEnumerationTimeoutSecs * 1000,
                  config_.dfuEnumerationPollMs, [this] {
                      return usb_sysfs::hasDeviceWithVidPid(
                          config_.usbSysfsRoot, config_.dfuVendorId,
                          config_.dfuProductId);
                  });
    if (enumerated)
    {
        lg2::info("USB DFU device {VID}:{PID} enumerated", "VID",
                  config_.dfuVendorId, "PID", config_.dfuProductId);
        return true;
    }

    out["Status"] = "Failed";
    out["Error"] = "HMC did not enumerate as USB DFU device " +
                   config_.dfuVendorId + ":" + config_.dfuProductId +
                   " within " +
                   std::to_string(config_.dfuEnumerationTimeoutSecs) + "s";
    out["ErrorCode"] = code(USBDFURecoveryErrorCode::DfuEnumerationFailed);
    return false;
}

bool UsbDfuRecovery::waitForRecoveryUBoot(nlohmann::json& out)
{
    if (config_.uBootDfuTimeoutSecs <= 0)
    {
        return true;
    }
    lg2::info("Waiting up to {T}s for recovery U-Boot to expose the DFU flash "
              "targets",
              "T", config_.uBootDfuTimeoutSecs);
    const std::vector<std::string> argv = {config_.dfuUtilPath, "-l"};
    const bool ready = pollUntil(
        config_.uBootDfuTimeoutSecs * 1000, config_.uBootDfuPollSecs * 1000,
        [&] {
            auto listing =
                usbdfu::process::capture(argv, config_.dfuListTimeoutSecs);
            return listing &&
                   listing->find("name=\"recovery_") != std::string::npos;
        });
    if (ready)
    {
        lg2::info("Recovery U-Boot DFU targets available");
        return true;
    }
    return setFailure(
        out,
        "Recovery U-Boot did not expose the recovery_* DFU targets "
        "within " +
            std::to_string(config_.uBootDfuTimeoutSecs) + "s",
        USBDFURecoveryErrorCode::RecoveryUBootDfuTimeout);
}

bool UsbDfuRecovery::isProgrammingComplete() const
{
    // No observable signal today: recovery U-Boot detaches its DFU gadget
    // before programming and returns to its prompt afterwards without
    // re-enumerating, and the HMC console is not routed to the BMC on this
    // platform.  Returning false makes waitForProgrammingComplete() run to
    // its ceiling, which is the current (correct but slow) behaviour.
    return false;
}

void UsbDfuRecovery::waitForProgrammingComplete()
{
    const int settleSecs = std::max(0, config_.postFlashSettleSecs);
    const int pollSecs = std::max(1, config_.postFlashPollSecs);

    lg2::info("Waiting up to {T}s for recovery U-Boot to authenticate and "
              "program SPI (polling every {P}s)",
              "T", settleSecs, "P", pollSecs);

    if (pollUntil(settleSecs * 1000, pollSecs * 1000,
                  [this] { return isProgrammingComplete(); }))
    {
        lg2::info("Recovery U-Boot reported programming complete");
        return;
    }
    lg2::info("Post-flash settle ceiling of {T}s reached", "T", settleSecs);
}

bool UsbDfuRecovery::assertRecoveryMode(nlohmann::json& out)
{
    out.clear();

    lg2::info("Asserting HMC recovery mode");

    if (!setRecoveryStrap(1, out))
    {
        out["ErrorCode"] = code(USBDFURecoveryErrorCode::GPIOAssertFailed);
        return false;
    }

    if (!waitForDfuEnumeration(out))
    {
        return false;
    }

    out["Status"] = "Successful";
    return true;
}

bool UsbDfuRecovery::deassertRecoveryMode(nlohmann::json& out)
{
    out.clear();

    lg2::info("Deasserting HMC recovery mode");

    // Re-acquire with recovery=0, then release immediately -- GPIOs are only
    // needed long enough to let the reset pulse observe the deasserted strap.
    if (!setRecoveryStrap(0, out))
    {
        out["ErrorCode"] = code(USBDFURecoveryErrorCode::GPIODeassertFailed);
        return false;
    }

    session_.release();

    out["Status"] = "Successful";
    return true;
}

bool UsbDfuRecovery::runPreliminaryBundle(
    const std::vector<std::filesystem::path>& bundle, nlohmann::json& out)
{
    out.clear();

    lg2::info("Running preliminary DFU bundle ({COUNT} binaries)", "COUNT",
              bundle.size());

    for (size_t i = 0; i < bundle.size(); ++i)
    {
        lg2::info("[{SEQ}/{TOT}] dfu-util -D {FILE}", "SEQ", i + 1, "TOT",
                  bundle.size(), "FILE", bundle[i].filename().string());

        nlohmann::json s;
        if (!runDfuDownload(DfuTransfer::BundleStage, bundle[i].string(), s))
        {
            out["Status"] = "Failed";
            out["FailedStep"] = static_cast<int>(i + 1);
            out["FailedBinary"] = bundle[i].filename().string();
            out["Error"] = s.value("Error", "dfu-util download failed");
            out["ErrorCode"] = code(USBDFURecoveryErrorCode::BundleSendFailed);
            return false;
        }

        if (i + 1 < bundle.size())
        {
            lg2::debug("Sleeping {DELAY}s before next bundle step", "DELAY",
                       config_.bundleStepDelaySecs);
            std::this_thread::sleep_for(
                std::chrono::seconds(config_.bundleStepDelaySecs));
        }
    }

    out["Status"] = "Successful";
    return true;
}

// static
bool UsbDfuRecovery::prepareFlashImage(const std::string& imagePath,
                                       std::uintmax_t lengthBytes,
                                       StagedImage& staged, nlohmann::json& out)
{
    std::error_code ec;
    const std::filesystem::path src(imagePath);

    // Existence is a precondition checked by preflightChecks(); file_size()
    // reports a missing or unreadable image here anyway.
    const auto size = std::filesystem::file_size(src, ec);
    if (ec)
    {
        return setFailure(out, "Cannot stat firmware image " + imagePath +
                                   ": " + ec.message());
    }

    staged.path = src;
    staged.sizeBytes = size;
    staged.temporary = false;

    if (lengthBytes == 0 || size <= lengthBytes)
    {
        return true;
    }

    // dfu-util has no download-length option, so stage a truncated copy next
    // to the image (same filesystem as the PLDM upload directory).
    std::filesystem::path tmp = src;
    tmp += ".dfu";
    std::filesystem::copy_file(
        src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        return setFailure(out, "Failed to stage truncated firmware image " +
                                   tmp.string() + ": " + ec.message());
    }
    staged.path = tmp;
    staged.temporary = true;

    std::filesystem::resize_file(tmp, lengthBytes, ec);
    if (ec)
    {
        return setFailure(out, "Failed to truncate staged firmware image " +
                                   tmp.string() + ": " + ec.message());
    }
    staged.sizeBytes = lengthBytes;

    lg2::info("Staged truncated firmware image {PATH}: sending {SENT} of "
              "{TOTAL} bytes (persistent tail excluded)",
              "PATH", tmp.string(), "SENT", lengthBytes, "TOTAL", size);
    return true;
}

bool UsbDfuRecovery::flashFirmware(const std::string& imagePath,
                                   nlohmann::json& out)
{
    out.clear();

    StagedImage staged;
    if (!prepareFlashImage(imagePath, config_.flashLengthBytes, staged, out))
    {
        out["ErrorCode"] = code(USBDFURecoveryErrorCode::FirmwareFlashFailed);
        return false;
    }

    lg2::info("Flashing firmware SPI image: dfu-util -a {ALT} -D {PATH} "
              "({BYTES} bytes)",
              "ALT", config_.dfuAltSetting, "PATH", staged.path.string(),
              "BYTES", staged.sizeBytes);

    nlohmann::json s;
    if (!runDfuDownload(DfuTransfer::FirmwareImage, staged.path.string(), s))
    {
        return setFailure(out,
                          s.value("Error", "dfu-util firmware flash failed"),
                          USBDFURecoveryErrorCode::FirmwareFlashFailed);
    }

    out["Status"] = "Successful";
    out["DfuAlt"] = config_.dfuAltSetting;
    out["FlashedBytes"] = staged.sizeBytes;

    // The transfer only streams the image into the HMC's DRAM.  Recovery
    // U-Boot begins authenticating and programming SPI when the DFU session
    // ends, so it must be told the transfer is finished.  A bare `-e` fails
    // with "More than one DFU capable USB device found" because recovery
    // U-Boot exposes recovery_cs0/cs1/both and fw_logs_cs0/cs1, so the alt
    // setting is always passed.
    if (config_.detachAfterFlash)
    {
        lg2::info("Detaching DFU session so recovery U-Boot programs SPI");
        nlohmann::json detach;
        if (!runDfuUtil({"-a", config_.dfuAltSetting, "-e"},
                        config_.dfuListTimeoutSecs, detach))
        {
            return setFailure(out,
                              detach.value("Error",
                                           "Failed to detach the DFU session; "
                                           "recovery U-Boot will not program "
                                           "SPI"),
                              USBDFURecoveryErrorCode::FirmwareFlashFailed);
        }
    }
    return true;
}

bool UsbDfuRecovery::preflightChecks(
    const std::string& fwspiImage,
    const std::vector<std::filesystem::path>& bundle, nlohmann::json& out) const
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fwspiImage, ec))
    {
        return setFailure(out, "Firmware image not found: " + fwspiImage,
                          USBDFURecoveryErrorCode::FirmwareFlashFailed);
    }
    if (bundle.empty())
    {
        return setFailure(out, "Preliminary bundle is empty",
                          USBDFURecoveryErrorCode::InvalidConfiguration);
    }
    for (const auto& binPath : bundle)
    {
        if (!std::filesystem::is_regular_file(binPath, ec))
        {
            return setFailure(out,
                              "Bundle binary not found: " + binPath.string(),
                              USBDFURecoveryErrorCode::BundleSendFailed);
        }
    }
    return true;
}

bool UsbDfuRecovery::performFullRecovery(
    const std::string& fwspiImage,
    const std::vector<std::filesystem::path>& bundle, nlohmann::json& out)
{
    out.clear();
    auto& steps = out["Steps"];

    // Fail before touching any GPIO if the inputs are not all present.
    if (!preflightChecks(fwspiImage, bundle, out))
    {
        steps["Preflight"] = "Failed";
        return false;
    }
    steps["Preflight"] = "Successful";

    {
        nlohmann::json s;
        if (!assertRecoveryMode(s))
        {
            if (s.value("ErrorCode", std::uint8_t{0}) ==
                code(USBDFURecoveryErrorCode::DfuEnumerationFailed))
            {
                // Straps were driven but the HMC never showed up: best-effort
                // return to normal boot so it is not left in recovery.
                nlohmann::json cleanup;
                deassertRecoveryMode(cleanup);
            }
            return failStep(out, "AssertRecovery", s,
                            "assertRecoveryMode failed",
                            USBDFURecoveryErrorCode::GPIOAssertFailed);
        }
        steps["AssertRecovery"] = "Successful";
        steps["ResetPulse1"] = "Successful";
        steps["DfuEnumeration"] = "Successful";
    }

    {
        nlohmann::json s;
        if (!runPreliminaryBundle(bundle, s))
        {
            nlohmann::json cleanup;
            deassertRecoveryMode(cleanup);
            return failStep(out, "PreliminaryBundle", s,
                            "Preliminary bundle failed",
                            USBDFURecoveryErrorCode::BundleSendFailed);
        }
        steps["PreliminaryBundle"] = "Successful";
    }

    {
        nlohmann::json w;
        if (!waitForRecoveryUBoot(w))
        {
            nlohmann::json cleanup;
            deassertRecoveryMode(cleanup);
            return failStep(out, "RecoveryUBoot", w,
                            "recovery U-Boot DFU targets not found",
                            USBDFURecoveryErrorCode::RecoveryUBootDfuTimeout);
        }
        steps["RecoveryUBoot"] = "Successful";
    }

    {
        nlohmann::json s;
        if (!flashFirmware(fwspiImage, s))
        {
            // Best-effort: deassert recovery before returning.
            nlohmann::json cleanup;
            deassertRecoveryMode(cleanup);
            return failStep(out, "FlashFirmware", s, "Firmware flash failed",
                            USBDFURecoveryErrorCode::FirmwareFlashFailed);
        }
        steps["FlashFirmware"] = "Successful";
        out["DfuAlt"] = s.value("DfuAlt", config_.dfuAltSetting);
        out["FlashedBytes"] = s.value("FlashedBytes", std::uintmax_t{0});
    }

    // flashFirmware() only detaches -- and so only starts programming --
    // when detachAfterFlash is set.  On the --no-detach path recovery U-Boot
    // is still sitting in its DFU session, so there is nothing to wait for.
    if (config_.detachAfterFlash)
    {
        waitForProgrammingComplete();
    }

    {
        nlohmann::json s;
        if (!deassertRecoveryMode(s))
        {
            return failStep(out, "DeassertRecovery", s,
                            "deassertRecoveryMode failed",
                            USBDFURecoveryErrorCode::GPIODeassertFailed);
        }
        steps["DeassertRecovery"] = "Successful";
        steps["ResetPulse2"] = "Successful";
    }

    out["Status"] = "Successful";
    return true;
}

bool resolvePackageComponents(const std::filesystem::path& packageDir,
                              PackageContents& contents, nlohmann::json& out)
{
    contents = PackageContents{};
    std::error_code ec;

    if (!std::filesystem::is_directory(packageDir, ec))
    {
        return UsbDfuRecovery::setFailure(
            out, "Recovery package directory not found: " + packageDir.string(),
            USBDFURecoveryErrorCode::PackageIncomplete);
    }

    for (const auto& component : usbdfu::componentMap)
    {
        // PLDM extracts component 0x10 to ".../16/<file>" (decimal id).
        const auto componentDir = packageDir / std::to_string(component.id);
        std::filesystem::path file;
        std::size_t regularFiles = 0;
        for (auto it = std::filesystem::directory_iterator(componentDir, ec),
                  end = std::filesystem::directory_iterator();
             !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec))
            {
                file = it->path();
                ++regularFiles;
            }
        }
        // directory_iterator order is unspecified, so taking the first of
        // several files would pick a different blob run to run.
        if (regularFiles > 1)
        {
            return UsbDfuRecovery::setFailure(
                out,
                std::format("Recovery package component 0x{:X} ({}) has {} "
                            "files under {}; expected exactly one",
                            component.id, component.name, regularFiles,
                            componentDir.string()),
                USBDFURecoveryErrorCode::PackageIncomplete);
        }
        if (file.empty() && component.optional)
        {
            lg2::info("Optional package component 0x{ID} {NAME} not "
                      "present; stage skipped",
                      "ID", lg2::hex, component.id, "NAME", component.name);
            continue;
        }
        if (file.empty())
        {
            return UsbDfuRecovery::setFailure(
                out,
                std::format("Recovery package component 0x{:X} ({}) missing: "
                            "no file under {}",
                            component.id, component.name,
                            componentDir.string()),
                USBDFURecoveryErrorCode::PackageIncomplete);
        }
        lg2::debug("Package component 0x{ID} {NAME}: {FILE}", "ID", lg2::hex,
                   component.id, "NAME", component.name, "FILE", file.string());
        if (component.role == usbdfu::ComponentRole::FirmwareImage)
        {
            contents.firmwareImage = file;
        }
        else
        {
            contents.bundle.push_back(file);
        }
    }
    return true;
}

