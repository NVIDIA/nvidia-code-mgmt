# USB DFU Recovery Tool (HMC / AST2700)

`usbdfu-recovery-tool` recovers the AST2700 Host Management Controller (HMC) on
the P4102 / vr-nvl-bmc-ast2700-a2 platform over USB DFU from the BMC. It is
built together with `usb-dfu-recovery`, the systemd worker that runs the same
sequence from the Redfish/PLDM update path.

The recovery sequence:

- Drive the HMC SPI mux so the HMC owns its SPI flash, assert the recovery strap
  (GPIO) and pulse reset so the BootROM enumerates as a USB DFU device (VID:PID
  `2245:2700`), then wait until that device is visible in sysfs.
- Push the ordered preliminary bundle of boot blobs (Caliptra firmware, SoC
  manifest, MCU runtime, DP firmware, DDR training images, BL31, OP-TEE, U-Boot)
  to the BootROM via `dfu-util`, staging the recovery U-Boot that exposes the
  SPI flash over DFU. The bundle travels inside the recovery PLDM package
  together with the SPI image, one component each.
- Transfer the firmware SPI image via `dfu-util -a recovery_cs0 -D` (the primary
  chip; `recovery_both` writes both from one authenticated transfer) and end the
  DFU session with `dfu-util -a recovery_cs0 -e`. Only the first `0x03F30000`
  bytes are sent so the persistent tail (FW logs, VSN, VRoT debug token)
  survives recovery.
- Wait for recovery U-Boot to authenticate and program (the slow part, about 15
  minutes for one chip-select; the tool waits up to 20), then deassert the
  recovery strap and pulse reset so the HMC boots normally.

## Components

| Component                        | Role                                                                                                                                                                                                      |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `usbdfu-recovery-tool`           | Standalone CLI (this README). No D-Bus dependency; reads the GPIO line names and strap polarities from the Entity Manager recovery configuration file (`--json`).                                         |
| `usb-dfu-recovery`               | systemd worker started by `usb-dfu-recovery@.service` with the extracted package directory as its only argument. Resolves the components, reads GPIO names from Entity Manager and logs Redfish messages. |
| `code-manager -u USBDFURecovery` | Item-updater (`com.Nvidia.USBDFURecovery.Updater.service`) that receives the PLDM package and starts the worker unit.                                                                                     |

## Before You Run

Use this tool only when HMC recovery is required. Asserting the recovery strap
and pulsing reset will interrupt the running HMC and should be done during an
approved maintenance or recovery window.

Run the tool from the BMC shell as root. The BMC shell must have access to the
GPIO lines and the USB bus connected to the HMC.

Prerequisites:

- `dfu-util` installed at the configured path (default `/usr/bin/dfu-util`).
- The recovery inputs: either an extracted recovery PLDM package
  (`--package-dir`, see "Recovery package layout") or, for lab use, the images
  listed in send order (`--images`).
- An Entity Manager recovery configuration naming the strap lines and their
  polarities, at the configured path or given with `--json`. The lines it names
  must be accessible via libgpiod on the BMC.
- The HMC USB recovery port connected to the BMC (enumerated as `2245:2700` when
  in DFU mode; verify with `lsusb`).

### Recovery configuration

The CLI reads the same Entity Manager configuration the `usb-dfu-recovery`
worker reads over D-Bus, so a lab run uses the platform's own wiring rather than
hand-typed line names:

```json
{
  "Exposes": [
    {
      "Name": "FW_HMC_0",
      "Type": "USBDFURecovery",
      "RecoveryGpioName": "HMC_RECOVERY_R-O",
      "RecoveryGpioPolarity": "ActiveHigh",
      "ResetGpioName": "HMC_RST_R_L-O",
      "ResetGpioPolarity": "ActiveLow",
      "SpiMuxGpioName": "HMC_SPI_MUX_R_SEL-O"
    }
  ]
}
```

The two polarity properties are optional and default to `ActiveHigh` for the
recovery strap and `ActiveLow` for reset. `--device` selects an entry when the
configuration exposes more than one.

## Build and Install

If the product image already contains `/usr/bin/usbdfu-recovery-tool`, skip this
section.

From the repository root, enable USB DFU recovery support when configuring
Meson:

```bash
meson setup build \
  -DUSBDFU_RECOVERY_SUPPORT=enabled \
  -DUSBDFU_RECOVERY_SUPPORTED_MODEL=Nvidia:USBDFURecovery:<UUID>
meson compile -C build usbdfu-recovery-tool usb-dfu-recovery
meson install -C build
```

Build options (all under `USBDFU_*` in `meson_options.txt`):

| Option                              | Default                 | Meaning                                                     |
| ----------------------------------- | ----------------------- | ----------------------------------------------------------- |
| `USBDFU_UTIL_PATH`                  | `/usr/bin/dfu-util`     | dfu-util binary                                             |
| `USBDFU_RECOVERY_DFU_ALT`           | `recovery_cs0`          | Chip-select written; `recovery_both` writes both            |
| `USBDFU_RECOVERY_FLASH_LENGTH`      | `66256896` (0x03F30000) | Image bytes sent to DFU; `0` sends the whole file           |
| `USBDFU_RECOVERY_CONFIG_PATH`       | Entity Manager config   | Recovery configuration the CLI reads (`--json`)             |
| `USBDFU_RECOVERY_POST_FLASH_SETTLE` | `1200`                  | Programming wait per chip-select, in seconds                |
| `USBDFU_RECOVERY_TIMEOUT`           | `5400`                  | Redfish task timeout in seconds (see "Timing and timeouts") |

Build dependencies: `libgpiodcxx`, `nlohmann_json`, `CLI11`, `phosphor-logging`
(CLI); additionally `sdbusplus` and `phosphor-dbus-interfaces` (worker).

## Command Summary

```bash
# From an extracted recovery package (same inputs as the Redfish path)
usbdfu-recovery-tool PerformRecovery --package-dir <UUID dir> [options]

# From a lab drop: bundle stages in send order, SPI image last
usbdfu-recovery-tool PerformRecovery \
    --images <stage1> <stage2> ... <stageN> <spi-image> \
    [options]

# options:
    [--dfu-util <path>] \
    [--dfu-alt recovery_cs0|recovery_cs1|recovery_both] \
    [--flash-length <bytes>] \
    [--bundle-step-delay <secs>] \
    [--no-detach] [--post-flash-settle <secs>] \
    [--json <config.json>] [--device <name>] \
    [--enum-timeout <secs>] \
    [-v|--verbose]

usbdfu-recovery-tool AssertRecovery \
    [--json <config.json>] [--device <name>] \
    [--enum-timeout <secs>]

usbdfu-recovery-tool DeassertRecovery \
    [--json <config.json>] [--device <name>]
```

All commands print a JSON object to stdout. Non-zero exit status means failure;
failures carry `"Status": "Failed"`, an `"Error"` description and a numeric
`"ErrorCode"` (see "Error codes"). Diagnostics go to stderr (and to the
journal), never to stdout, so the JSON can be piped to `jq` directly. By default
only warnings and errors are shown; `-v` adds the info-level progress messages.
On a terminal lg2 prefixes each line with its syslog priority, e.g.
`<6> HMC recovery status: ...`.

## Recommended Recovery Flow

1. Verify the HMC USB port is not yet in DFU mode: `lsusb` should not show
   `2245:2700`.
2. Copy the recovery inputs to the BMC: either the extracted recovery package
   directory, or the bundle stages and the SPI image.
3. Run `PerformRecovery`. The tool performs the full sequence end-to-end.
4. After completion, `lsusb` should no longer show `2245:2700` and the HMC
   should boot normally.

Example:

```bash
# Copy firmware and lab bundle to BMC
scp hmc_fwspi.bin root@<bmc-ip>:/tmp/
scp -r hmc_bundle/ root@<bmc-ip>:/tmp/hmc_bundle

# Run full recovery: bundle stages in send order, SPI image last
usbdfu-recovery-tool PerformRecovery \
    --images /tmp/hmc_bundle/ast2700-caliptra-fw.bin \
             /tmp/hmc_bundle/ast2700-soc-manifest.bin \
             ... \
             /tmp/hmc_bundle/u-boot.bin \
             /tmp/hmc_fwspi.bin
```

## PerformRecovery

`PerformRecovery` drives the entire USB DFU recovery sequence:

1. Preflight: resolves the inputs (package components, or the `--images` list)
   and verifies the firmware image and every bundle binary exist before any GPIO
   is touched.
2. Drives `HMC_SPI_MUX_R_SEL-O=0` (HMC owns its SPI flash) and
   `HMC_RECOVERY_R-O=1` (recovery strap), then pulses `HMC_RST_R_L-O` to reset
   the HMC into BootROM DFU mode.
3. Polls sysfs until a USB device with VID:PID `2245:2700` appears (default
   timeout 15 s, `--enum-timeout`). If it never appears the straps are released
   and the command fails with `DfuEnumerationFailed`.
4. Pushes each bundle binary in order via `dfu-util -D`, with an inter-step
   delay (default 10 s, `--bundle-step-delay`).
5. Polls `dfu-util -l` until recovery U-Boot exposes its `recovery_*` DFU
   targets (default 180 s, 5 s interval). Recovery U-Boot first erases the
   debug-token and PDS slots on both SPI chips, which takes far longer than the
   lab script's `sleep 10`; if the targets never appear the command fails with
   `RecoveryUBootDfuTimeout` and releases the straps.
6. Transfers the firmware SPI image via `dfu-util -a <alt> -D <image>`, sending
   only the first `--flash-length` bytes (see "Firmware image handling"), then
   ends the DFU session with `dfu-util -a <alt> -e`. The transfer only places
   the image in the HMC's DRAM; ending the session is what makes recovery U-Boot
   authenticate the manifests and program SPI.
7. Waits for programming to finish (default ceiling 1200 s per chip-select, so
   2400 s for `recovery_both`, `--post-flash-settle`). Recovery U-Boot gives the
   BMC no completion signal today, so the tool waits the full ceiling. Measured:
   901 s for CS0 and 794 s for CS1. Resetting the HMC earlier aborts programming
   and leaves the flash partly written.
8. Deasserts `HMC_RECOVERY_R-O=0` and pulses reset so the HMC boots normally.

Example output on success:

```json
{
  "Status": "Successful",
  "DfuAlt": "recovery_cs0",
  "FlashedBytes": 66256896,
  "Steps": {
    "Preflight": "Successful",
    "AssertRecovery": "Successful",
    "ResetPulse1": "Successful",
    "DfuEnumeration": "Successful",
    "PreliminaryBundle": "Successful",
    "RecoveryUBoot": "Successful",
    "FlashFirmware": "Successful",
    "DeassertRecovery": "Successful",
    "ResetPulse2": "Successful"
  }
}
```

Example output on failure:

```json
{
  "Status": "Failed",
  "Error": "HMC did not enumerate as USB DFU device 2245:2700 within 15s",
  "ErrorCode": 5,
  "Steps": {
    "Preflight": "Successful",
    "AssertRecovery": "Failed"
  }
}
```

When a `dfu-util` invocation fails, its exit code and the last lines of its
combined stdout/stderr are added to the result as `DfuUtilExitCode` /
`DfuUtilOutput` and logged to the journal; dfu-util prints all of its
diagnostics (`No DFU capable USB device available`,
`Cannot set alternate interface`) on stderr.

On any failure after the recovery strap was asserted, the tool performs a
best-effort `DeassertRecovery` so the HMC is not left strapped into recovery.

### Side effects of a recovery attempt

Recovery is destructive from the moment the preliminary bundle reaches recovery
U-Boot, not only when the SPI image is written:

- Recovery U-Boot erases the debug-token and PDS slots on **both** SPI chips
  before it exposes the DFU targets. A run that fails after the bundle stage
  (for example with `RecoveryUBootDfuTimeout`) therefore leaves the HMC without
  debug tokens even though no image was flashed; re-provision them afterwards.
- The reset pulse on `HMC_RST_R_L-O` resets the AST2700 SoC but not Caliptra.
  After a failed attempt, an aborted transfer, or when the HMC has to be taken
  out of recovery without a successful flash, AC power-cycle the HMC (chassis
  power) before retrying; `DeassertRecovery` alone does not return Caliptra to a
  clean state.
- The recovery kit must match the board: the signing keys and the HSM variant of
  the bundle and image are checked by Caliptra / BootROM, so a package built for
  a different variant fails at the first bundle stage.

## Error codes

`ErrorCode` values are shared with the `usb-dfu-recovery` worker, which maps
them to Redfish `ResourceEvent.1.0.ResourceErrorsDetected` messages
(`recovery_tool/common/message_registry.hpp`, `USBDFURecoveryErrorCode`).

| Code | Name                      | Meaning                                                                                 |
| ---- | ------------------------- | --------------------------------------------------------------------------------------- |
| 1    | `GPIOAssertFailed`        | Could not drive the SPI mux / recovery strap or pulse reset                             |
| 2    | `BundleSendFailed`        | A bundle binary is missing or its `dfu-util -D` failed                                  |
| 3    | `FirmwareFlashFailed`     | Firmware image missing, could not be staged, or the final `dfu-util` failed             |
| 4    | `GPIODeassertFailed`      | Could not deassert the recovery strap or pulse reset into normal boot                   |
| 5    | `DfuEnumerationFailed`    | `2245:2700` did not appear on the USB bus after asserting recovery                      |
| 6    | `InvalidConfiguration`    | `--images` list too short, or (worker) Entity Manager GPIO names missing                |
| 7    | `PackageIncomplete`       | A component of the recovery package is missing (see "Recovery package layout")          |
| 8    | `RecoveryUBootDfuTimeout` | Recovery U-Boot never exposed the `recovery_*` DFU targets after the preliminary bundle |

## AssertRecovery

`AssertRecovery` sets the SPI MUX, asserts the recovery strap, pulses reset and
waits for the HMC to enumerate as a USB DFU device, without performing any DFU
transfer. Use this for manual recovery workflows or debugging. If the device
does not enumerate the command fails but leaves the straps asserted so the state
can be inspected; run `DeassertRecovery` to return to normal boot.

```bash
usbdfu-recovery-tool AssertRecovery
```

Example output:

```json
{
  "Status": "Successful"
}
```

After this command, `lsusb` shows `2245:2700`. Push bundle binaries and flash
the image manually with `dfu-util`, then run `DeassertRecovery`.

## DeassertRecovery

`DeassertRecovery` deasserts the recovery strap and pulses reset to return the
HMC to normal boot. Run after manual DFU operations are complete.

```bash
usbdfu-recovery-tool DeassertRecovery
```

Example output:

```json
{
  "Status": "Successful"
}
```

## Recovery package layout

The recovery PLDM package (`.fwpkg`) carries the whole recovery kit, the same
way the USB RCM recovery package carries its components: one component per
preliminary bundle stage, then the SPI image. Bundle and image therefore always
match, and nothing firmware-related has to be baked into the BMC image. PLDM
extracts every component to
`<IMG_UPLOAD_DIR>/USBDFURecovery/<UUID>/<component id, decimal>/<file>`; the
worker sends them in the order below. The table is defined once in
`recovery_tool/common/usbdfu_components.hpp` and must match the package
definition owned by the release team.

| Order | Component ID | Name                 | Role                              |
| ----- | ------------ | -------------------- | --------------------------------- |
| 1     | 0x1 (`1`)    | `Caliptra_FW`        | bundle stage                      |
| 2     | 0x2 (`2`)    | `SoC_Manifest`       | bundle stage                      |
| 3     | 0x3 (`3`)    | `MCU_Runtime`        | bundle stage                      |
| 4     | 0x4 (`4`)    | `DP_FW`              | bundle stage                      |
| 5     | 0x5 (`5`)    | `DDR_PMU_Train_IMEM` | bundle stage                      |
| 6     | 0x6 (`6`)    | `DDR_PMU_Train_DMEM` | bundle stage                      |
| 7     | 0x7 (`7`)    | `BL31`               | bundle stage                      |
| 8     | 0x8 (`8`)    | `TEE`                | bundle stage                      |
| 9     | 0x9 (`9`)    | `U-Boot`             | bundle stage                      |
| 10    | 0xA (`10`)   | `Zephyr_SSP`         | bundle stage, optional            |
| 11    | 0xB (`11`)   | `Zephyr_TSP`         | bundle stage, optional            |
| 12    | 0x10 (`16`)  | `HMC_SPI_Image`      | SPI image flashed via `--dfu-alt` |

A component directory must contain exactly one regular file; a missing mandatory
component fails the task with `PackageIncomplete` before any GPIO is touched.
The two Zephyr components are optional: a package carries either 9 or 11 bundle
stages depending on the HMC firmware drop. The P4102 A2 / DDR5 `verified` lab
bundle has 9 stages (`ast2700-caliptra-fw*.bin`, `ast2700-soc-manifest.bin`,
`ast2700-mcu-runtime.bin`, `dp_fw.bin`, `ddr5_pmu_train_imem.bin`,
`ddr5_pmu_train_dmem.bin`, `bl31-ast2700.bin`, `tee-raw.bin`, `u-boot.bin`);
newer drops add `zephyr-aspeed-ssp.bin` and `zephyr-aspeed-tsp.bin` after
U-Boot, for 11 stages. Absent optional components are logged and skipped; the
sequence of the remaining stages is unchanged. The "Sunda Firmware Recovery
Flows" design document (section 7.1.2) lists the EVB / DDR4 bundle with 13
entries (extra `ddr4_2d_*` blobs); if a platform needs those, the table and the
package definition grow together.

## Lab drop (CLI only)

For manual recovery, pass the images to `PerformRecovery --images` in send
order: the bundle stages first, the HMC SPI image last. The tool sends every
element but the last through `dfu-util -D`, then flashes the final one.

```bash
usbdfu-recovery-tool PerformRecovery \
    --images ast2700-caliptra-fw-v1.2-ecc-lms_patched.bin \
             ast2700-soc-manifest.bin \
             ast2700-mcu-runtime.bin \
             dp_fw.bin \
             ddr5_pmu_train_imem.bin \
             ddr5_pmu_train_dmem.bin \
             bl31-ast2700.bin \
             tee-raw.bin \
             u-boot.bin \
             hmc_fwspi.bin
```

`--bundle-step-delay` overrides the inter-stage delay.

## Firmware image handling

- **Alt setting.** Recovery U-Boot exposes `recovery_cs0`, `recovery_cs1` and
  `recovery_*` DFU targets (see `dfu-util -l` while it is waiting for the
  image). The default `recovery_cs0` programs the primary chip only;
  `recovery_both` programs both from one authenticated transfer, as required by
  the Sunda recovery design; use `--dfu-alt recovery_cs0` (or the
  `USBDFU_RECOVERY_DFU_ALT` build option) to program the primary chip only.
- **Truncation.** A normal 64 MiB signed image can be passed as is. Only bytes
  `[0, 0x03F30000)` are sent: the tool stages a truncated copy next to the image
  (`<image>.dfu`, removed afterwards) because the persistent tail holds the FW
  logs, VSN and debug-token regions that must survive recovery. This temporarily
  needs ~63 MiB of extra space on the image's filesystem. Pass
  `--flash-length 0` to send the whole file.
- **Detach.** After the transfer the tool sends `dfu-util -a <alt> -e` to end
  the DFU session, which is what starts programming. The alt setting is repeated
  because recovery U-Boot also exposes `fw_logs_cs0/cs1`, so a bare `-e` fails
  with "More than one DFU capable USB device found". `-R` is not used: it resets
  the HMC as soon as the image reaches DRAM, before anything is programmed, and
  dfu-util then exits non-zero even on a good download. `--no-detach` suppresses
  the detach (debug only: recovery U-Boot keeps the session open and programs
  nothing).

## Timing and timeouts

All values are conservative upper bounds taken from the P4102 lab material and
from bench measurements on P4102 (Sep 2026), not specification minimums:

| Constant                        | Value              | Source                                                                                              |
| ------------------------------- | ------------------ | --------------------------------------------------------------------------------------------------- |
| Strap settle / reset pulse      | 1 s / 1 s          | HMC USB DFU recovery runbook                                                                        |
| DFU enumeration timeout         | 15 s (poll 500 ms) | runbook shows ~2 s until `lsusb` lists `2245:2700`; 5x margin                                       |
| Delay between bundle stages     | 10 s               | lab recovery script (`sleep 10`); `--bundle-step-delay` overrides                                   |
| Per-stage dfu-util timeout      | 120 s              | bundle blobs are < 2 MB each                                                                        |
| Wait for recovery U-Boot DFU    | 180 s (poll 5 s)   | bench: U-Boot erases both chips' debug-token/PDS slots first; `sleep 10` is too short               |
| Final transfer dfu-util timeout | 300 s              | bounds only the DRAM transfer (seconds in practice)                                                 |
| Post-flash programming wait     | 1200 s (poll 10 s) | bench: U-Boot programs SPI after the session ends, 901 s for CS0; ceiling, no completion signal yet |

End-to-end estimate: strap + enumeration 5-20 s, bundle 9-11 x (~2 s transfer
plus 10 s delay) = ~2-2.5 min, recovery U-Boot erase and enumeration up to 3
min, image transfer a few seconds, programming wait 40 min (the fixed ceiling;
the measured programming time is ~15 min for one chip-select), deassert ~5 s,
i.e. **roughly 45 minutes** regardless of alt setting until recovery U-Boot can
signal completion. The `USBDFU_RECOVERY_TIMEOUT` build option (default 3600 s)
bounds the whole Redfish task; if it expires the item-updater stops the worker
unit, which terminates the running `dfu-util`, and the task fails with a timeout
message. The per-step `dfu-util` timeouts only protect against a hung transfer
and are not individually bounded by the task timeout.

## Troubleshooting

- **`DfuEnumerationFailed` / `2245:2700` not visible after `AssertRecovery`**:
  Confirm the USB cable between the BMC and HMC is connected and that the GPIO
  names match the platform (`gpiofind HMC_RECOVERY_R-O`). Check the strap with
  `gpioget $(gpiofind HMC_RECOVERY_R-O)`.
- **`GPIO line not found`**: Confirm the GPIO names match the platform. If
  another process holds the GPIO, the tool will report the error and release all
  held lines.
- **`dfu-util failed`**: The result's `DfuUtilOutput` (and the journal) carry
  the last lines dfu-util printed, e.g. `No DFU capable USB device available` or
  `Cannot set alternate interface: LIBUSB_ERROR_OTHER`. Run `dfu-util -l` to
  confirm the device is enumerated and, for the final flash, that the configured
  alt setting is listed.
- **`RecoveryUBootDfuTimeout`**: the bundle was accepted but recovery U-Boot
  never exposed `recovery_cs0/cs1/both`. Its erase of the debug-token/PDS slots
  has likely already run (see "Side effects of a recovery attempt"); AC
  power-cycle the HMC before retrying and check the recovery U-Boot's USB/DFU
  support with the HMC firmware team.
- **`Bundle binary not found`**: Confirm every path given to `--images` exists.
- **`Recovery package component ... missing`** (`PackageIncomplete`): the
  extracted package lacks a component directory or file; rebuild the `.fwpkg`
  with every entry of the component table.
- **Worker reports `No USB DFU recovery device configured`**: Entity Manager
  does not expose an `xyz.openbmc_project.Configuration.USBDFURecovery` object
  (see below), or it was not yet published when the worker ran.

## Redfish / PLDM Integration

When deployed with the `com.Nvidia.USBDFURecovery.Updater` service, the recovery
can be triggered via the Redfish UpdateService by POSTing the USB DFU recovery
`.fwpkg` package. The PLDM daemon extracts every component to
`<IMG_UPLOAD_DIR>/USBDFURecovery/<UUID>/<component id>/<file>`; the item-updater
watches those directories and, on activation, starts
`usb-dfu-recovery@<escaped UUID directory>.service`. That unit runs

```text
/usr/bin/usb-dfu-recovery <UUID directory>
```

Only the package directory is passed. The worker obtains everything else from
the package and the platform:

- Bundle stages and the SPI image from the package components (see "Recovery
  package layout").
- GPIO strap names from the Entity Manager configuration object of interface
  `xyz.openbmc_project.Configuration.USBDFURecovery`, for example:

  ```json
  {
    "Name": "FW_HMC_0",
    "Type": "USBDFURecovery",
    "RecoveryGpioName": "HMC_RECOVERY_R-O",
    "ResetGpioName": "HMC_RST_R_L-O",
    "SpiMuxGpioName": "HMC_SPI_MUX_R_SEL-O"
  }
  ```

  The object name is used as the device name in Redfish messages. If the object
  or any of the three properties is missing the worker fails without touching
  any GPIO.

- dfu-util path, alt setting and flash length from the `USBDFU_*` build options.

Redfish message registry entries logged by the worker:

| Message                                    | When                                                         |
| ------------------------------------------ | ------------------------------------------------------------ |
| `NvidiaUpdate.1.0.RecoveryStarted`         | Configuration and package validated, sequence starting       |
| `NvidiaUpdate.1.0.RecoverySuccessful`      | Sequence completed                                           |
| `ResourceEvent.1.0.ResourceErrorsDetected` | Any failure, with the message/resolution for the `ErrorCode` |

The worker's exit code drives the systemd unit result and therefore the Redfish
task state (`Update.1.2.TransferFailed` on failure).
