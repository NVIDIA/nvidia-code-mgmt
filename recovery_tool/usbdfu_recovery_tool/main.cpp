// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "config.h"

#include "default_config.hpp"
#include "perform_dfu_recovery.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

// The GPIO names are platform data and live in the Entity Manager
// configuration read by the D-Bus worker (usb-dfu-recovery); the standalone
// CLI has no D-Bus access, so they must be given on the command line.
static void addGpioOptions(CLI::App* cmd, UsbDfuRecovery::Config& cfg)
{
    cmd->add_option("--gpio-recovery", cfg.recoveryGpioName,
                    "GPIO line name for the HMC recovery strap, e.g. "
                    "HMC_RECOVERY_R-O")
        ->required();
    cmd->add_option("--gpio-reset", cfg.resetGpioName,
                    "GPIO line name for HMC reset, e.g. HMC_RST_R_L-O")
        ->required();
    cmd->add_option("--gpio-spi-mux", cfg.spiMuxGpioName,
                    "GPIO line name for the HMC SPI mux select, e.g. "
                    "HMC_SPI_MUX_R_SEL-O")
        ->required();
}

static void addEnumerationOptions(CLI::App* cmd, UsbDfuRecovery::Config& cfg)
{
    cmd->add_option(
        "--enum-timeout", cfg.dfuEnumerationTimeoutSecs,
        "Seconds to wait for the HMC to enumerate as USB DFU "
        "device 2245:2700 after reset (default: " +
            std::to_string(usbdfu::timing::dfuEnumerationTimeoutSecs) + ")");
}

// Diagnostics go to stderr through lg2 (and to the journal); the JSON result
// is the only thing written to stdout.  Without -v the CLI shows warnings and
// errors only, so `usbdfu-recovery-tool <cmd> | jq` and a terminal session
// both see a clean result.
static void addVerboseFlag(CLI::App* cmd, bool& verbose)
{
    cmd->add_flag("-v,--verbose", verbose,
                  "Also print info-level diagnostics to stderr");
}

static int emit(const nlohmann::json& output, bool ok)
{
    std::cout << output.dump(2) << '\n';
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int fail(nlohmann::json& output, const std::string& error,
                USBDFURecoveryErrorCode code)
{
    output["Status"] = "Failed";
    output["Error"] = error;
    output["ErrorCode"] = static_cast<uint8_t>(code);
    return emit(output, false);
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        CLI::App app{"USB DFU Recovery Tool for the HMC (AST2700)"};
        app.require_subcommand(1);

        UsbDfuRecovery::Config cfg = usbdfu::makeDefaultConfig();
        std::string fwspiImage;
        std::string bundleDir;
        std::string manifestPath;
        std::string packageDir;
        bool noDetach = false;
        bool verbose = false;

        auto* performCmd = app.add_subcommand(
            "PerformRecovery",
            "Full USB DFU recovery: assert recovery, wait for DFU "
            "enumeration, push the preliminary bundle, flash the firmware, "
            "deassert recovery. Inputs come either from an extracted "
            "recovery package (--package-dir) or from a firmware image plus "
            "a bundle directory with a manifest (--firmware/--bundle-dir).");
        auto* fwOpt = performCmd->add_option(
            "-f,--firmware", fwspiImage,
            "Path to the HMC firmware SPI image to flash");
        auto* bundleOpt = performCmd->add_option(
            "-b,--bundle-dir", bundleDir,
            "Directory containing the preliminary DFU bundle binaries "
            "listed in the manifest");
        auto* manifestOpt = performCmd->add_option(
            "-m,--manifest", manifestPath,
            "JSON manifest listing the ordered bundle binary filenames "
            "(default: <bundle-dir>/manifest.json)");
        performCmd
            ->add_option("-p,--package-dir", packageDir,
                         "Extracted recovery PLDM package directory "
                         "(<UUID>/<component id>/<file> layout, as used by "
                         "the usb-dfu-recovery worker)")
            ->excludes(fwOpt)
            ->excludes(bundleOpt)
            ->excludes(manifestOpt);
        performCmd->add_option("--dfu-util", cfg.dfuUtilPath,
                               "Path to the dfu-util binary on the BMC "
                               "(default: " USBDFU_UTIL_PATH ")");
        performCmd->add_option(
            "--dfu-alt", cfg.dfuAltSetting,
            "dfu-util alt setting for the final flash: recovery_both or "
            "recovery_cs0 (default: " USBDFU_RECOVERY_DFU_ALT ")");
        performCmd->add_option(
            "--flash-length", cfg.flashLengthBytes,
            "Number of image bytes sent to DFU; 0 sends the whole file "
            "(default: " +
                std::to_string(USBDFU_RECOVERY_FLASH_LENGTH) + ")");
        performCmd->add_flag("--no-detach", noDetach,
                             "Do not detach the DFU session after the final "
                             "transfer; recovery U-Boot will NOT program SPI");
        performCmd->add_option(
            "--post-flash-settle", cfg.postFlashSettleSecs,
            "Seconds to wait after the DFU transfer for recovery U-Boot to "
            "authenticate and program SPI before deasserting recovery "
            "(default: " +
                std::to_string(USBDFU_RECOVERY_POST_FLASH_SETTLE) + ")");
        addGpioOptions(performCmd, cfg);
        addEnumerationOptions(performCmd, cfg);
        addVerboseFlag(performCmd, verbose);

        auto* assertCmd = app.add_subcommand(
            "AssertRecovery",
            "Set SPI MUX, assert the recovery strap, pulse reset and wait for "
            "the HMC to enumerate as a USB DFU device");
        addGpioOptions(assertCmd, cfg);
        addEnumerationOptions(assertCmd, cfg);
        addVerboseFlag(assertCmd, verbose);

        auto* deassertCmd = app.add_subcommand(
            "DeassertRecovery",
            "Deassert the HMC recovery strap and pulse reset into normal boot");
        addGpioOptions(deassertCmd, cfg);
        addVerboseFlag(deassertCmd, verbose);

        CLI11_PARSE(app, argc, argv);
        if (!verbose)
        {
            // lg2 reads LG2_LOG_LEVEL (syslog numbering) at its first use;
            // 4 = warning.  An explicit environment setting wins.
            ::setenv("LG2_LOG_LEVEL", "4", 0);
        }
        cfg.detachAfterFlash = !noDetach;

        nlohmann::json output;

        if (performCmd->parsed())
        {
            std::vector<std::filesystem::path> bundle;
            std::string image;

            if (!packageDir.empty())
            {
                PackageContents contents;
                if (!resolvePackageComponents(packageDir, contents, output))
                {
                    return emit(output, false);
                }
                bundle = contents.bundle;
                image = contents.firmwareImage.string();
            }
            else
            {
                if (fwspiImage.empty() || bundleDir.empty())
                {
                    return fail(output,
                                "--firmware and --bundle-dir are required "
                                "unless --package-dir is given",
                                USBDFURecoveryErrorCode::InvalidConfiguration);
                }
                if (manifestPath.empty())
                {
                    manifestPath =
                        (std::filesystem::path(bundleDir) / "manifest.json")
                            .string();
                }
                int manifestDelay = cfg.bundleStepDelaySecs;
                nlohmann::json manifestErr;
                auto names = loadBundleManifest(manifestPath, manifestDelay,
                                                manifestErr);
                if (names.empty())
                {
                    return fail(output,
                                manifestErr.value(
                                    "Error", "Failed to load bundle manifest"),
                                USBDFURecoveryErrorCode::InvalidConfiguration);
                }
                cfg.bundleStepDelaySecs = manifestDelay;
                for (const auto& name : names)
                {
                    bundle.push_back(std::filesystem::path(bundleDir) / name);
                }
                image = fwspiImage;
            }

            if (verbose)
            {
                lg2::info("Bundle: {COUNT} binaries, step delay {DELAY}s, "
                          "image {IMG}, flash alt {ALT}, flash length {LEN}",
                          "COUNT", bundle.size(), "DELAY",
                          cfg.bundleStepDelaySecs, "IMG", image, "ALT",
                          cfg.dfuAltSetting, "LEN", cfg.flashLengthBytes);
            }

            UsbDfuRecovery recovery(cfg);
            return emit(output,
                        recovery.performFullRecovery(image, bundle, output));
        }

        UsbDfuRecovery recovery(cfg);
        if (assertCmd->parsed())
        {
            return emit(output, recovery.assertRecoveryMode(output));
        }
        if (deassertCmd->parsed())
        {
            return emit(output, recovery.deassertRecoveryMode(output));
        }
        return EXIT_FAILURE; // unreachable: require_subcommand(1)
    }
    catch (const std::exception& e)
    {
        nlohmann::json err;
        err["Status"] = "Failed";
        err["Error"] = e.what();
        err["ErrorCode"] = static_cast<std::uint8_t>(
            USBDFURecoveryErrorCode::InvalidConfiguration);
        std::cout << err.dump(2) << '\n';
        return EXIT_FAILURE;
    }
}
