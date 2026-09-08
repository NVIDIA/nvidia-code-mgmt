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
// The worker reads this configuration from Entity Manager over D-Bus; the
// standalone CLI has no D-Bus access and reads the same file directly.
static void addConfigOptions(CLI::App* cmd, std::string& configPath,
                             std::string& deviceName)
{
    cmd->add_option("-j,--json", configPath,
                    "Entity Manager recovery configuration providing the "
                    "GPIO line names and strap polarities "
                    "(default: " USBDFU_RECOVERY_CONFIG_PATH ")");
    cmd->add_option("--device", deviceName,
                    "Name of the USBDFURecovery entry to use when the "
                    "configuration exposes more than one");
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

struct StatusOptions
{
    CLI::Option* functionalUsbId{nullptr};
    CLI::Option* usbPort{nullptr};
};

// Status-probe options shared by PerformRecovery and GetRecoveryStatus.  Both
// are bound to plain strings rather than cfg: the recovery configuration is
// loaded after parsing and supplies the defaults, so a value given on the
// command line has to be applied on top of it (see main), not under it.
static StatusOptions addStatusOptions(CLI::App* cmd,
                                      std::string& functionalUsbId,
                                      std::string& usbPort)
{
    StatusOptions opts;
    opts.functionalUsbId = cmd->add_option(
        "--functional-usb-id", functionalUsbId,
        "USB vid:pid of a normally booted HMC on the recovery "
        "link; overrides FunctionalUsbId from the recovery "
        "configuration. Unset: HMC health cannot be determined");
    opts.usbPort = cmd->add_option(
        "--usb-port", usbPort,
        "Kernel USB port path of the HMC, e.g. 1-1.3; overrides "
        "UsbPort from the recovery configuration (default: any port)");
    return opts;
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
        std::vector<std::string> images;
        std::string packageDir;
        std::string configPath{USBDFU_RECOVERY_CONFIG_PATH};
        std::string deviceName;
        bool noDetach = false;
        std::string functionalUsbId; // CLI overrides of the config values
        std::string usbPort;
        bool noVerify = false;
        bool verbose = false;

        auto* performCmd = app.add_subcommand(
            "PerformRecovery",
            "Full USB DFU recovery: assert recovery, wait for DFU "
            "enumeration, push the preliminary bundle, flash the firmware, "
            "deassert recovery. Takes either an extracted recovery package "
            "(--package-dir) or an ordered list of images (--images).");
        auto* imagesOpt = performCmd->add_option(
            "-i,--images", images,
            "Recovery images in send order: the preliminary bundle stages "
            "followed by the HMC firmware SPI image last "
            "(e.g. caliptra.bin ... u-boot.bin hmc_spi.bin)");
        performCmd
            ->add_option("-p,--package-dir", packageDir,
                         "Extracted recovery PLDM package directory "
                         "(<UUID>/<component id>/<file> layout, as used by "
                         "the usb-dfu-recovery worker)")
            ->excludes(imagesOpt);
        performCmd
            ->add_option(
                "--bundle-step-delay", cfg.bundleStepDelaySecs,
                "Seconds to wait between bundle stages "
                "(default: " +
                    std::to_string(usbdfu::timing::bundleStepDelaySecs) + ")")
            ->check(CLI::NonNegativeNumber);
        performCmd->add_option("--dfu-util", cfg.dfuUtilPath,
                               "Path to the dfu-util binary on the BMC "
                               "(default: " USBDFU_UTIL_PATH ")");
        performCmd
            ->add_option("--dfu-alt", cfg.dfuAltSetting,
                         "Which SPI chip-select recovery U-Boot programs: "
                         "recovery_cs0 (primary chip only), recovery_cs1 "
                         "(secondary chip only) or recovery_both (both chips "
                         "from one authenticated transfer) "
                         "(default: " USBDFU_RECOVERY_DFU_ALT ")")
            ->check(CLI::IsMember(
                {"recovery_cs0", "recovery_cs1", "recovery_both"}));
        auto performStatusOpts =
            addStatusOptions(performCmd, functionalUsbId, usbPort);
        performCmd->add_flag(
            "--force", cfg.forceRecovery,
            "Drive the recovery strap and flash regardless of the status "
            "probe (also re-flashes a healthy HMC)");
        performCmd->add_flag("--no-verify", noVerify,
                             "Do not wait for the HMC to leave DFU mode after "
                             "recovery");
        performCmd->add_option(
            "--verify-timeout", cfg.postRecoveryVerifyTimeoutSecs,
            "Seconds to wait for the HMC to leave DFU mode after recovery "
            "(default: " +
                std::to_string(usbdfu::timing::postRecoveryVerifyTimeoutSecs) +
                ")");
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
        addConfigOptions(performCmd, configPath, deviceName);
        addEnumerationOptions(performCmd, cfg);
        addVerboseFlag(performCmd, verbose);

        auto* statusCmd = app.add_subcommand(
            "GetRecoveryStatus",
            "Report the HMC recovery status from its USB descriptors: In "
            "Recovery (BootROM DFU 2245:2700 or recovery U-Boot DFU targets), "
            "Not in Recovery (functional identity present) or Unknown");
        auto statusStatusOpts =
            addStatusOptions(statusCmd, functionalUsbId, usbPort);
        statusCmd->add_option("--dfu-util", cfg.dfuUtilPath,
                              "Path to the dfu-util binary on the BMC "
                              "(default: " USBDFU_UTIL_PATH ")");
        addConfigOptions(statusCmd, configPath, deviceName);
        addVerboseFlag(statusCmd, verbose);

        auto* assertCmd = app.add_subcommand(
            "AssertRecovery",
            "Set SPI MUX, assert the recovery strap, pulse reset and wait for "
            "the HMC to enumerate as a USB DFU device");
        addConfigOptions(assertCmd, configPath, deviceName);
        addEnumerationOptions(assertCmd, cfg);
        addVerboseFlag(assertCmd, verbose);

        auto* deassertCmd = app.add_subcommand(
            "DeassertRecovery",
            "Deassert the HMC recovery strap and pulse reset into normal boot");
        addConfigOptions(deassertCmd, configPath, deviceName);
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

        if (!loadRecoveryConfig(configPath, deviceName, cfg, output))
        {
            return emit(output, false);
        }
        cfg.verifyAfterRecovery = !noVerify;

        // An explicit --functional-usb-id replaces the configured value; an
        // empty one ("") probes as if the platform had none.
        if (performStatusOpts.usbPort->count() > 0 ||
            statusStatusOpts.usbPort->count() > 0)
        {
            cfg.usbPortFilter = usbPort; // "" = any port
        }
        if (performStatusOpts.functionalUsbId->count() > 0 ||
            statusStatusOpts.functionalUsbId->count() > 0)
        {
            cfg.functionalUsbId = functionalUsbId.empty()
                                      ? std::nullopt
                                      : usbdfu::UsbId::parse(functionalUsbId);
            if (!functionalUsbId.empty() && !cfg.functionalUsbId)
            {
                return fail(output,
                            "--functional-usb-id must be vvvv:pppp (hex), got "
                            "'" +
                                functionalUsbId + "'",
                            USBDFURecoveryErrorCode::InvalidConfiguration);
            }
        }

        if (statusCmd->parsed())
        {
            UsbDfuRecovery recovery(cfg);
            output = recovery.probeStatus().toJson();
            // A derived status (even Unknown) is a successful query.
            return emit(output, true);
        }

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
                // The SPI image is sent last, so it is the final element and
                // everything before it is a bundle stage in send order.
                if (images.size() < 2)
                {
                    return fail(output,
                                "--images needs at least one bundle stage "
                                "and the firmware image, unless "
                                "--package-dir is given",
                                USBDFURecoveryErrorCode::InvalidConfiguration);
                }
                for (std::size_t i = 0; i + 1 < images.size(); ++i)
                {
                    bundle.emplace_back(images[i]);
                }
                image = images.back();
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
            const auto outcome =
                recovery.performRecovery(image, bundle, output);
            return emit(output, outcome != UsbDfuRecovery::Outcome::Failed);
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
