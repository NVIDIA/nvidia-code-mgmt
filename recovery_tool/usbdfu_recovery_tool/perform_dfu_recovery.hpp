// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#pragma once

// Header-only use: USBDFURecoveryErrorCode is shared with the D-Bus worker
// (usb-dfu-recovery), which maps it to Redfish task messages.  The standalone
// CLI does not link message_registry.cpp or sdbusplus.
#include "message_registry.hpp"
#include "usbdfu_components.hpp"

#include <gpiod.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace usbdfu
{

/** USB identity of the AST2700 BootROM when it waits for DFU recovery. */
constexpr const char* bootRomDfuVendorId = "2245";
constexpr const char* bootRomDfuProductId = "2700";
constexpr const char* defaultUsbSysfsRoot = "/sys/bus/usb/devices";

/**
 * Default dfu-util alt setting for the final SPI flash.  recovery U-Boot
 * exposes recovery_cs0 (primary chip only), recovery_cs1 and recovery_both
 * (both chips from one authenticated transfer).  The Sunda recovery design
 * writes the image to both chips.
 */
constexpr const char* defaultDfuAltSetting = "recovery_both";

/**
 * Number of firmware image bytes sent to DFU: [0, 0x03F30000).  The region
 * above this offset holds persistent data (FW logs, VSN, debug tokens) that
 * must survive recovery, so -- exactly like the lab usb_recovery_ddr5.sh
 * script -- the tail is never transferred.  0 disables truncation.
 */
constexpr std::uintmax_t defaultFlashLengthBytes = 0x03F30000;

/**
 * Timing constants.  Sources:
 *  (a) dgx/sunda/tools/lab: ast27X0_a2_SDK11_0_p4102_usb_recovery/
 *      usb_recovery_ddr5.sh -- `sleep 10` between preliminary bundle stages
 *      and `sleep 10` before the final flash while recovery U-Boot
 *      re-enumerates in DFU mode.
 *  (b) "HMC USB DFU Firmware Recovery" runbook (same directory) -- 1 s
 *      settle after driving the straps, 1 s reset pulse, ~2 s until `lsusb`
 *      lists 2245:2700.
 *  (c) Transfer sizes: bundle blobs are < 2 MB each (a few seconds per
 *      dfu-util -D); the firmware image is ~63 MB and recovery U-Boot writes
 *      it to both SPI chips and verifies it by SHA-384 readback.
 *  (d) P4102 bench measurements, Sep 2026 (lab it-evidence-p4102blue-hmc-dfu).
 * All values are upper bounds / conservative settle times, not spec minimums.
 */
namespace timing
{
constexpr int strapSettleSecs = 1;            // (b)
constexpr int resetPulseSecs = 1;             // (b)
constexpr int dfuEnumerationTimeoutSecs = 15; // (b) typ. 2-3 s, 5x margin
constexpr int dfuEnumerationPollMs = 500;
constexpr int bundleStepDelaySecs = 10;    // (a) manifest step_delay_secs wins
constexpr int bundleStepTimeoutSecs = 120; // (c) per dfu-util -D of one blob
// (d) P4102 bench, Sep 2026: recovery U-Boot needs well over the script's
//     `sleep 10` to boot, erase the debug-token/PDS slots on both chips and
//     enumerate its DFU targets, so it is polled for instead of slept for.
constexpr int uBootDfuTimeoutSecs = 180;
constexpr int uBootDfuPollSecs = 5;
// (d) `dfu-util -D` only streams the image into the HMC's DRAM and returns
//     within seconds.  Recovery U-Boot authenticates the manifests and
//     programs SPI *after* the DFU session ends, and that is the slow part:
//     measured on P4102 at 901 s (CS0) + 794 s (CS1), ~75-85 KB/s, for
//     recovery_both.  The HMC must not be reset until both chip-selects
//     report "programming and readback verified", so the settle covers the
//     programming phase, not the transfer.
// The transfer itself only streams into DRAM: a full 63 MiB recovery_both
// download completes in well under a minute over USB 2.0.  2400 s was sized
// for programming, which postFlashSettleSecs now covers, so this only needs
// enough margin for a slow link.
constexpr int flashTimeoutSecs = 300;
// Upper bound, not an unconditional sleep: waitForProgrammingComplete()
// polls for a completion signal and returns as soon as one is seen.  The
// BMC currently has no view of the HMC console and recovery U-Boot does not
// re-enumerate after programming, so today the poll always runs to the
// ceiling.  Wire a signal into isProgrammingComplete() when one exists.
constexpr int postFlashSettleSecs = 2400; // (d)
constexpr int postFlashPollSecs = 10;
constexpr int dfuListTimeoutSecs = 30; // dfu-util -l
} // namespace timing

} // namespace usbdfu

/**
 * RAII holder for the GPIO lines that must remain asserted for the duration of
 * the recovery session (from assertRecoveryMode through deassertRecoveryMode).
 * Lines are released automatically when the session is destroyed.
 */
struct GpioSession
{
    gpiod::line spiMuxLine;
    gpiod::line recoveryLine;

    GpioSession() = default;

    /** Non-copyable; lines are owned exclusively by this session. */
    GpioSession(const GpioSession&) = delete;
    GpioSession& operator=(const GpioSession&) = delete;

    ~GpioSession()
    {
        release();
    }

    /**
     * Release both lines and drop the handles so that calling release()
     * again (or the destructor) never releases a line twice.
     */
    void release() noexcept
    {
        try
        {
            if (recoveryLine)
                recoveryLine.release();
            if (spiMuxLine)
                spiMuxLine.release();
        }
        catch (...)
        {}
        recoveryLine = gpiod::line();
        spiMuxLine = gpiod::line();
    }

    explicit operator bool() const
    {
        return static_cast<bool>(spiMuxLine);
    }
};

/**
 * Core USB DFU recovery logic for the HMC (AST2700).
 *
 * Mirrors the sequence of the P4102 lab runbook / usb_recovery_ddr5.sh but
 * runs natively on the BMC:
 *   1. Drive SPI-MUX=0 (HMC owns its SPI flash), RECOVERY=1, pulse RESET
 *   2. Wait for the BootROM to enumerate as USB DFU device 2245:2700
 *   3. Push the preliminary boot bundle through dfu-util (staged loader)
 *   4. Flash the firmware SPI image via dfu-util alt setting recovery_both
 *   5. Deassert the recovery strap and pulse RESET into normal boot
 *
 * Every failure path sets out["Status"] = "Failed", out["Error"] and
 * out["ErrorCode"] (a USBDFURecoveryErrorCode) so callers can map the result
 * to a Redfish message registry entry.
 */
class UsbDfuRecovery
{
  public:
    struct Config
    {
        /** GPIO used to assert/deassert recovery mode (active high) */
        std::string recoveryGpioName;
        /** GPIO used to pulse HMC reset (active low) */
        std::string resetGpioName;
        /** GPIO used to hand SPI flash control to the HMC (drive low) */
        std::string spiMuxGpioName;
        /** Absolute path to dfu-util binary on the BMC */
        std::string dfuUtilPath;
        /** dfu-util alt setting for the final SPI flash */
        std::string dfuAltSetting{usbdfu::defaultDfuAltSetting};
        /** Firmware image bytes sent to DFU; 0 = whole file */
        std::uintmax_t flashLengthBytes{usbdfu::defaultFlashLengthBytes};
        /** Pass -R so the device leaves DFU after the final download */
        bool detachAfterFlash{true};
        /** sysfs root scanned for the BootROM DFU device (tests override) */
        std::string usbSysfsRoot{usbdfu::defaultUsbSysfsRoot};
        std::string dfuVendorId{usbdfu::bootRomDfuVendorId};
        std::string dfuProductId{usbdfu::bootRomDfuProductId};
        /** Timing (seconds unless noted); see usbdfu::timing for provenance */
        int bundleStepDelaySecs{usbdfu::timing::bundleStepDelaySecs};
        int bundleStepTimeoutSecs{usbdfu::timing::bundleStepTimeoutSecs};
        int dfuEnumerationTimeoutSecs{
            usbdfu::timing::dfuEnumerationTimeoutSecs};
        int dfuEnumerationPollMs{usbdfu::timing::dfuEnumerationPollMs};
        /** Wait for recovery U-Boot's recovery_* DFU targets; 0 = do not
         *  wait (tests) */
        int uBootDfuTimeoutSecs{usbdfu::timing::uBootDfuTimeoutSecs};
        int uBootDfuPollSecs{usbdfu::timing::uBootDfuPollSecs};
        int flashTimeoutSecs{usbdfu::timing::flashTimeoutSecs};
        int postFlashSettleSecs{usbdfu::timing::postFlashSettleSecs};
        int postFlashPollSecs{usbdfu::timing::postFlashPollSecs};
        int dfuListTimeoutSecs{usbdfu::timing::dfuListTimeoutSecs};
    };

    /**
     * Which dfu-util download is being performed.  Selects the alt setting,
     * extra dfu-util flags, timeout and the label used in error messages.
     */
    enum class DfuTransfer
    {
        /** Preliminary bundle blob to the BootROM (default alt setting). */
        BundleStage,
        /** Final SPI image to recovery U-Boot (Config::dfuAltSetting, -R). */
        FirmwareImage,
    };

    /**
     * Firmware image as handed to dfu-util.  When the image is longer than
     * Config::flashLengthBytes a truncated copy is staged next to it and
     * removed when this object goes out of scope.
     */
    struct StagedImage
    {
        std::filesystem::path path;
        std::uintmax_t sizeBytes{0};
        bool temporary{false};

        StagedImage() = default;
        StagedImage(const StagedImage&) = delete;
        StagedImage& operator=(const StagedImage&) = delete;
        ~StagedImage()
        {
            if (temporary)
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    };

    explicit UsbDfuRecovery(const Config& config);

    /**
     * Drive SPI-MUX and the recovery strap, pulse reset, then wait until the
     * BootROM enumerates as a USB DFU device.  On enumeration timeout the
     * straps are left asserted (so a manual AssertRecovery can be debugged);
     * performFullRecovery() deasserts them best-effort.
     */
    bool assertRecoveryMode(nlohmann::json& out);

    /**
     * Deassert the recovery strap, pulse reset into normal boot and release
     * the GPIO lines.
     */
    bool deassertRecoveryMode(nlohmann::json& out);

    /**
     * Push the ordered preliminary bundle to the BootROM via dfu-util -D,
     * sleeping config_.bundleStepDelaySecs between binaries.
     */
    bool runPreliminaryBundle(const std::vector<std::filesystem::path>& bundle,
                              nlohmann::json& out);

    /**
     * Flash the firmware SPI image: stage a copy truncated to
     * config_.flashLengthBytes when needed, then
     * dfu-util -a <dfuAltSetting> [-R] -D <image>.
     */
    bool flashFirmware(const std::string& imagePath, nlohmann::json& out);

    /**
     * Run the full recovery sequence end-to-end.  Verifies the image and the
     * bundle binaries exist before touching any GPIO.
     */
    bool performFullRecovery(const std::string& fwspiImage,
                             const std::vector<std::filesystem::path>& bundle,
                             nlohmann::json& out);

    /**
     * Poll config_.usbSysfsRoot until a device with dfuVendorId:dfuProductId
     * appears, for at most dfuEnumerationTimeoutSecs.
     */
    bool waitForDfuEnumeration(nlohmann::json& out);

    /**
     * Poll `dfu-util -l` until recovery U-Boot exposes its recovery_* DFU
     * targets, for at most uBootDfuTimeoutSecs (0 = return immediately).
     */
    bool waitForRecoveryUBoot(nlohmann::json& out);

    /**
     * @brief Has recovery U-Boot finished authenticating and programming?
     *
     * dfu-util returning only means the image reached the HMC's DRAM.  There
     * is currently no signal the BMC can observe while U-Boot programs SPI,
     * so this returns false and the caller falls back to its ceiling.
     */
    bool isProgrammingComplete() const;

    /**
     * @brief Wait for programming to finish, bounded by postFlashSettleSecs.
     */
    void waitForProgrammingComplete();

    /** Last dfu-util command line that was executed (diagnostics / tests). */
    const std::string& lastCommand() const
    {
        return lastCommand_;
    }

    /**
     * Record a failure in `out` (Status, Error, ErrorCode).  Always returns
     * false so call sites can `return setFailure(...)`.
     */
    static bool setFailure(nlohmann::json& out, const std::string& error,
                           USBDFURecoveryErrorCode errorCode);
    /** Same, for sub-steps whose caller assigns the ErrorCode. */
    static bool setFailure(nlohmann::json& out, const std::string& error);

    /**
     * Record a failed step of the full sequence: marks `step` as Failed in
     * out["Steps"] and copies the sub-result's Error / ErrorCode into `out`
     * (falling back to `fallback` / `fallbackCode`).  Always returns false.
     */
    static bool failStep(nlohmann::json& out, const char* step,
                         const nlohmann::json& stepResult,
                         const std::string& fallback,
                         USBDFURecoveryErrorCode fallbackCode);

  private:
    /**
     * Poll `ready` immediately and then every `intervalMs` until it returns
     * true or `timeoutMs` has elapsed; the final nap is clamped so the last
     * poll lands exactly on the deadline.  A timeout of 0 polls once without
     * sleeping.  Returns whether `ready` became true.
     */
    static bool pollUntil(int timeoutMs, int intervalMs,
                          const std::function<bool()>& ready);

    Config config_;

    /**
     * RAII session holding SPI-MUX and recovery GPIO lines for the duration
     * of assertRecoveryMode -> deassertRecoveryMode.
     */
    GpioSession session_;

    std::string lastCommand_;

    /**
     * Request a GPIO line for output and drive it to value; returns the held
     * line (invalid on failure, with errorMsg set).
     */
    static gpiod::line requestOutputLine(const std::string& pinName, int value,
                                         std::string& errorMsg) noexcept;

    /** Pulse resetGpioName low then high to trigger a hard reset. */
    bool pulseReset() noexcept;

    /**
     * Set SPI-MUX low and recovery strap to recoveryValue, then pulse reset.
     * Acquires (or re-acquires) session_ GPIO lines.
     */
    bool setRecoveryStrap(int recoveryValue, nlohmann::json& out);

    /** Verify the firmware image and every bundle binary exist. */
    bool preflightChecks(const std::string& fwspiImage,
                         const std::vector<std::filesystem::path>& bundle,
                         nlohmann::json& out) const;

    /**
     * Resolve the file handed to dfu-util for the final flash, staging a
     * truncated copy when the image is longer than lengthBytes (0 = as is).
     */
    static bool prepareFlashImage(const std::string& imagePath,
                                  std::uintmax_t lengthBytes,
                                  StagedImage& staged, nlohmann::json& out);

    /** dfu-util invocation parameters derived from a DfuTransfer. */
    struct DfuTransferParams
    {
        std::string label;
        std::vector<std::string> dfuUtilArgs;
        int timeoutSecs;
    };

    /** Map a DfuTransfer to its label, dfu-util arguments and timeout. */
    DfuTransferParams transferParams(DfuTransfer transfer) const;

    /**
     * Download filePath with dfu-util using the parameters of `transfer`.
     * Checks file existence first; sets out["Error"] and returns false on
     * failure.
     */
    bool runDfuDownload(DfuTransfer transfer, const std::string& filePath,
                        nlohmann::json& out);

    /**
     * Run dfu-util with the given arguments; timeoutSecs caps the call via
     * timeout(1).
     */
    bool runDfuUtil(const std::vector<std::string>& dfuUtilArgs,
                    int timeoutSecs, nlohmann::json& out) noexcept;
};

/** Files of an extracted USB DFU recovery package, in DFU send order. */
struct PackageContents
{
    std::vector<std::filesystem::path> bundle;
    std::filesystem::path firmwareImage;
};

/**
 * Resolve the files of a PLDM-extracted recovery package.  `packageDir` is
 * the <UUID> directory; each component of usbdfu::componentMap must be
 * present as <packageDir>/<id decimal>/<single file>.  On error, writes a
 * description to out["Error"], sets out["ErrorCode"] =
 * USBDFURecoveryErrorCode::PackageIncomplete and returns false.
 */
bool resolvePackageComponents(const std::filesystem::path& packageDir,
                              PackageContents& contents, nlohmann::json& out);

/**
 * Load an ordered list of bundle binary filenames from a JSON manifest
 * (standalone CLI use with a lab bundle directory).
 * Expected format: { "binaries": ["a.bin", "b.bin", ...],
 *                    "step_delay_secs": N }
 * stepDelaySecs is set to usbdfu::timing::bundleStepDelaySecs unless the
 * manifest overrides it.  On error, writes a description to out["Error"] and
 * returns an empty vector.
 */
std::vector<std::string> loadBundleManifest(const std::string& manifestPath,
                                            int& stepDelaySecs,
                                            nlohmann::json& out);
