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
#include <optional>
#include <string>
#include <vector>

namespace usbdfu
{

/** USB identity of the AST2700 BootROM when it waits for DFU recovery. */
constexpr const char* bootRomDfuVendorId = "2245";
constexpr const char* bootRomDfuProductId = "2700";
constexpr const char* defaultUsbSysfsRoot = "/sys/bus/usb/devices";

/** Program the primary chip only: half the SPI writes, so half the
 *  programming time.  recovery_cs1 and recovery_both are also available;
 *  recovery_both writes both chips from one authenticated transfer. */
constexpr const char* defaultDfuAltSetting = "recovery_cs0";

/** Alt setting that programs both chips, and so needs twice the settle. */
constexpr const char* bothChipSelectsAlt = "recovery_both";

/** Anything but the canonical values returns nullopt, so the caller rejects
 *  the config rather than driving a strap the wrong way round. */
inline std::optional<bool> parseActiveLowPolarity(const std::string& value)
{
    if (value == "ActiveHigh")
    {
        return false;
    }
    if (value == "ActiveLow")
    {
        return true;
    }
    return std::nullopt;
}

constexpr int gpioActive(bool activeLow)
{
    return activeLow ? 0 : 1;
}

constexpr int gpioInactive(bool activeLow)
{
    return activeLow ? 1 : 0;
}

/**
 * Number of firmware image bytes sent to DFU: [0, 0x03F30000).  The region
 * above this offset holds persistent data (FW logs, VSN, debug tokens) that
 * must survive recovery, so the tail is never transferred.  0 disables
 * truncation.
 */
constexpr std::uintmax_t defaultFlashLengthBytes = 0x03F30000;

/**
 * Timing constants.  Upper bounds and conservative settle times taken from
 * the lab recovery script, the recovery runbook and bench measurements; they
 * are not spec minimums.
 */
namespace timing
{
constexpr int strapSettleSecs = 1;
constexpr int resetPulseSecs = 1;
constexpr int dfuEnumerationTimeoutSecs = 15; // typ. 2-3 s, 5x margin
constexpr int dfuEnumerationPollMs = 500;
constexpr int bundleStepDelaySecs = 10;    // --bundle-step-delay wins
constexpr int bundleStepTimeoutSecs = 120; // per dfu-util -D of one blob
// Recovery U-Boot boots, erases the debug-token/PDS slots on both chips and
// only then enumerates its DFU targets, so it is polled for, not slept for.
constexpr int uBootDfuTimeoutSecs = 180;
constexpr int uBootDfuPollSecs = 5;
// dfu-util -D only streams the image into DRAM and returns within seconds;
// recovery U-Boot authenticates and programs SPI after the session ends, so
// this bounds the transfer alone, whichever chip-selects are written.
constexpr int flashTimeoutSecs = 300;
// Upper bound per chip-select, not an unconditional sleep:
// waitForProgrammingComplete() polls and returns as soon as programming is
// reported done, and doubles this for recovery_both.  No signal is observable
// today, so the poll runs to the ceiling; wire one into
// isProgrammingComplete() when one exists.
constexpr int postFlashSettleSecs = 1200;
constexpr int postFlashPollSecs = 10;
constexpr int dfuListTimeoutSecs = 30; // dfu-util -l
} // namespace timing

} // namespace usbdfu

/** Holds the lines that stay asserted for the recovery session. */
struct GpioSession
{
    gpiod::line spiMuxLine;
    gpiod::line recoveryLine;

    GpioSession() = default;

    GpioSession(const GpioSession&) = delete;
    GpioSession& operator=(const GpioSession&) = delete;

    ~GpioSession()
    {
        release();
    }

    /** Drops the handles, so a second call never releases a line twice.
     *  Each release is guarded on its own: one throwing must not leave the
     *  other line requested while its handle is dropped. */
    void release() noexcept
    {
        if (recoveryLine)
        {
            try
            {
                recoveryLine.release();
            }
            catch (...)
            {}
        }
        if (spiMuxLine)
        {
            try
            {
                spiMuxLine.release();
            }
            catch (...)
            {}
        }
        recoveryLine = gpiod::line();
        spiMuxLine = gpiod::line();
    }

    explicit operator bool() const
    {
        return static_cast<bool>(spiMuxLine);
    }
};

/**
 * Core USB DFU recovery logic for the HMC (AST2700); see the README for the
 * recovery sequence.  Every failure path sets out["Status"], out["Error"]
 * and out["ErrorCode"] so callers can map the result to a Redfish message.
 */
class UsbDfuRecovery
{
  public:
    struct Config
    {
        std::string recoveryGpioName;
        std::string resetGpioName;
        std::string spiMuxGpioName;
        /** Strap polarities; from the recovery configuration */
        bool recoveryActiveLow{false};
        bool resetActiveLow{true};
        std::string dfuUtilPath;
        std::string dfuAltSetting{usbdfu::defaultDfuAltSetting};
        /** Bytes sent to DFU; 0 = whole file.  The tail above this offset
         *  holds data that must survive recovery. */
        std::uintmax_t flashLengthBytes{usbdfu::defaultFlashLengthBytes};
        /** End the DFU session after the transfer.  False leaves it open, so
         *  recovery U-Boot never programs SPI. */
        bool detachAfterFlash{true};
        std::string usbSysfsRoot{usbdfu::defaultUsbSysfsRoot};
        std::string dfuVendorId{usbdfu::bootRomDfuVendorId};
        std::string dfuProductId{usbdfu::bootRomDfuProductId};
        /** Seconds unless noted; defaults from usbdfu::timing */
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

    /** Selects alt setting, timeout and error label for a dfu-util -D. */
    enum class DfuTransfer
    {
        BundleStage,
        FirmwareImage,
    };

    /** A copy truncated to Config::flashLengthBytes is staged when needed
     *  and removed with this object. */
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

    /** On enumeration timeout the straps are left asserted, so a manual
     *  AssertRecovery can be debugged; performFullRecovery() deasserts. */
    bool assertRecoveryMode(nlohmann::json& out);

    bool deassertRecoveryMode(nlohmann::json& out);

    bool runPreliminaryBundle(const std::vector<std::filesystem::path>& bundle,
                              nlohmann::json& out);

    /** Transfers the image, then ends the DFU session so recovery U-Boot
     *  starts programming. */
    bool flashFirmware(const std::string& imagePath, nlohmann::json& out);

    /** Verifies every input exists before touching any GPIO. */
    bool performFullRecovery(const std::string& fwspiImage,
                             const std::vector<std::filesystem::path>& bundle,
                             nlohmann::json& out);

    bool waitForDfuEnumeration(nlohmann::json& out);

    /** 0 = return immediately. */
    bool waitForRecoveryUBoot(nlohmann::json& out);

    /** dfu-util returning only means the image reached DRAM.  No signal is
     *  observable while U-Boot programs SPI, so this returns false and the
     *  caller runs to its ceiling. */
    bool isProgrammingComplete() const;

    void waitForProgrammingComplete();

    const std::string& lastCommand() const
    {
        return lastCommand_;
    }

    /** Always returns false, so call sites can `return setFailure(...)`. */
    static bool setFailure(nlohmann::json& out, const std::string& error,
                           USBDFURecoveryErrorCode errorCode);
    static bool setFailure(nlohmann::json& out, const std::string& error);

    /** Copies the sub-result's Error/ErrorCode into `out`, falling back to
     *  the supplied pair.  Always returns false. */
    static bool failStep(nlohmann::json& out, const char* step,
                         const nlohmann::json& stepResult,
                         const std::string& fallback,
                         USBDFURecoveryErrorCode fallbackCode);

  private:
    /** Polls immediately, then every intervalMs.  The final nap is clamped
     *  to the deadline; a timeout of 0 polls once without sleeping. */
    static bool pollUntil(int timeoutMs, int intervalMs,
                          const std::function<bool()>& ready);

    Config config_;

    /** Held from assertRecoveryMode() to deassertRecoveryMode(). */
    GpioSession session_;

    std::string lastCommand_;

    static gpiod::line requestOutputLine(const std::string& pinName, int value,
                                         std::string& errorMsg) noexcept;

    /** Assert then release resetGpioName, per Config::resetActiveLow. */
    bool pulseReset() noexcept;

    /** Acquires (or re-acquires) the session_ lines, then pulses reset. */
    bool setRecoveryStrap(int recoveryValue, nlohmann::json& out);

    bool preflightChecks(const std::string& fwspiImage,
                         const std::vector<std::filesystem::path>& bundle,
                         nlohmann::json& out) const;

    /** Stages a truncated copy when the image exceeds lengthBytes
     *  (0 = as is). */
    static bool prepareFlashImage(const std::string& imagePath,
                                  std::uintmax_t lengthBytes,
                                  StagedImage& staged, nlohmann::json& out);

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
/**
 * Fill the GPIO names and strap polarities from an Entity Manager
 * configuration file -- the same file the worker reads over D-Bus, so a lab
 * run uses the platform's own wiring rather than hand-typed line names.
 *
 * `deviceName` selects one Exposes entry; empty takes the first
 * USBDFURecovery entry.  Polarities are optional and keep their defaults
 * when absent, but a present-and-unrecognized value is rejected.
 */
bool loadRecoveryConfig(const std::string& configPath,
                        const std::string& deviceName,
                        UsbDfuRecovery::Config& cfg, nlohmann::json& out);

bool resolvePackageComponents(const std::filesystem::path& packageDir,
                              PackageContents& contents, nlohmann::json& out);
