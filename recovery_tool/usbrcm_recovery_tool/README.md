# USB RCM Recovery Tool

`usbrcm-recovery-tool` is a standalone command-line tool for recovering NVIDIA
Vera CPU devices over USB RCM, also known as USB Recovery Mode. It is intended
to be run from the BMC by an operator or service engineer.

The tool can:

- Discover Vera USB RCM devices and report their recovery state as JSON.
- Read progress and error codes from the device.
- Determine whether a DOT blob is required for the device.
- Put supported platforms into USB RCM by driving board GPIOs.
- Transfer Vera recovery images to a selected USB port over the RCM bulk
  endpoint.

This README covers the standalone `usbrcm-recovery-tool` CLI only. The related
`usb-rcm-recovery` D-Bus/Redfish helper is built from the same sources but has a
different interface.

## Before You Run

Use this tool only when Vera CPU recovery is required. `SetForceRecovery`
asserts platform reset and changes boot pins so the CPU boots into USB RCM. That
interrupts the running CPU and should be done during an approved maintenance or
recovery window.

Run the tool from the BMC shell with privileges that allow USB and GPIO access.
BMC shells commonly run as `root`; otherwise, prefix the commands with `sudo`.

Prerequisites:

- A Vera device connected to the BMC over USB. The tool scans for NVIDIA VID:PID
  `0x0955:0x7410`.
- Permission to open and claim the USB device through libusb.
- `gpiofind` and `gpioset` from libgpiod on `PATH` if you use
  `SetForceRecovery`.
- The correct Vera recovery image files for the target platform.
- A DOT (Device Owner Token) blob file when `GetRecoveryStatus` reports
  `"DOT Blob Required": "Yes"`.

## Build And Install

If the product image already contains `/usr/bin/usbrcm-recovery-tool`, skip this
section.

From the repository root, enable USB RCM support when configuring Meson:

```bash
meson setup build -DUSBRCM_RECOVERY_SUPPORT=enabled
meson compile -C build usbrcm-recovery-tool
meson install -C build
```

The install path is controlled by Meson's `bindir`; on target images this is
typically `/usr/bin/usbrcm-recovery-tool`.

Typical build dependencies are `libusb-1.0`, `nlohmann_json`, `CLI11`, and
`phosphor-logging`.

## Command Summary

```bash
usbrcm-recovery-tool GetRecoveryStatus [-v|--verbose]
usbrcm-recovery-tool SetForceRecovery -c <c2|c1g2|c2g4>
usbrcm-recovery-tool PerformUSBRecovery -p <port-path> [-b <dot-blob>] \
  -i <image1> [<image2> ...] [-v|--verbose]
```

All commands print JSON to stdout when run without `-v`. Verbose mode prints
additional diagnostic text to stdout and stderr, so do not use `-v` when another
program needs to parse stdout as JSON.

## Recommended Recovery Flow

1. Check the current device state and note the USB port path.
2. If the device is not in USB RCM, force the platform into USB RCM.
3. Re-run status and confirm the selected port has
   `"Recovery Status": "In Recovery"` and an empty `"Error"` field.
4. Run `PerformUSBRecovery` with the port path and correctly ordered images.
5. Re-run status and confirm recovery completed or the CPU is no longer in
   recovery.
6. Follow the platform recovery procedure to return boot pins and reset state to
   normal operation. The standalone CLI does not expose a command to clear
   forced-recovery GPIOs after recovery.

Example:

```bash
# 1. Discover devices.
usbrcm-recovery-tool GetRecoveryStatus

# 2. Force USB RCM if required. Choose the config for the platform.
usbrcm-recovery-tool SetForceRecovery -c c1g2

# 3. Confirm the port path and DOT requirement after reset.
usbrcm-recovery-tool GetRecoveryStatus

# 4. Recover the device. Replace PORT and image paths with values for the system.
PORT=1-1.3
usbrcm-recovery-tool PerformUSBRecovery -p "$PORT" \
  -i \
  /path/to/ROM_IMG.bin \
  /path/to/L1PT.bin \
  /path/to/CSH_OEM_FWS.bin \
  /path/to/CALF_OEM_FWS.bin \
  /path/to/PSC_BCT.bin \
  /path/to/CSH_NV_FWS.bin \
  /path/to/OOBHUB_FW.bin \
  /path/to/PSCFW_T.bin \
  /path/to/PSCFW_D.bin

# 5. Verify.
usbrcm-recovery-tool GetRecoveryStatus
```

If `"DOT Blob Required"` is `"Yes"`, pass the DOT blob before `-i`:

```bash
PORT=1-1.3
usbrcm-recovery-tool PerformUSBRecovery -p "$PORT" \
  -b /path/to/dot_blob.bin \
  -i \
  /path/to/ROM_IMG.bin \
  /path/to/L1PT.bin \
  /path/to/CSH_OEM_FWS.bin \
  /path/to/CALF_OEM_FWS.bin \
  /path/to/PSC_BCT.bin \
  /path/to/CSH_NV_FWS.bin \
  /path/to/OOBHUB_FW.bin \
  /path/to/PSCFW_T.bin \
  /path/to/PSCFW_D.bin
```

## Image Order

`PerformUSBRecovery` sends image files exactly in the order listed after `-i`.
For a normal full Vera recovery, provide the recovery images in this order:

| Order | Image          | Description                           |
| ----- | -------------- | ------------------------------------- |
| 1     | `ROM_IMG`      | FMC CSH text and data                 |
| 2     | `L1PT`         | Support for streaming                 |
| 3     | `CSH_OEM_FWS`  | CSH for PSC BCT and Caliptra firmware |
| 4     | `CALF_OEM_FWS` | Caliptra firmware                     |
| 5     | `PSC_BCT`      | Uniform PSC BCT                       |
| 6     | `CSH_NV_FWS`   | CSH for OOBHUB and PSC firmware       |
| 7     | `OOBHUB_FW`    | OOBHUB firmware binary                |
| 8     | `PSCFW_T`      | PSC firmware text binary              |
| 9     | `PSCFW_D`      | PSC firmware data binary              |

If a platform-specific recovery procedure provides a different image list,
follow that procedure.

## GetRecoveryStatus

`GetRecoveryStatus` lists all Vera USB devices found by VID:PID and reports one
JSON entry per device.

Usage:

```bash
usbrcm-recovery-tool GetRecoveryStatus [-v|--verbose]
```

Example:

```bash
usbrcm-recovery-tool GetRecoveryStatus
```

Example output:

```json
[
  {
    "USB Port Path": "1-1.3",
    "Error": "",
    "CPU Instance": "0",
    "Recovery Status": "In Recovery",
    "DOT Blob Required": "No",
    "Boot Selection Info": "",
    "Last Progress Code": "0x00000000 -> ...",
    "Last Error Code": ""
  }
]
```

Important fields:

| Field                | Meaning                                                                      |
| -------------------- | ---------------------------------------------------------------------------- |
| `USB Port Path`      | Port path to pass to `PerformUSBRecovery -p`.                                |
| `Recovery Status`    | `Recovery Complete`, `In Recovery`, `Not in Recovery`, or `Unknown`.         |
| `DOT Blob Required`  | `Yes`, `No`, or `Unknown` based on ECID.                                     |
| `Last Progress Code` | Last decoded progress code read from the CPU progress queue.                 |
| `Last Error Code`    | Last decoded error code, if present.                                         |
| `Error`              | Per-device issue found while reading status. Empty means no issue was found. |

Do not start `PerformUSBRecovery` for a device that reports
`"Recovery Status": "Unknown"` or has a non-empty `"Error"` field. If
`"DOT Blob Required"` is `"Unknown"`, fix the USB/control-read issue or follow
the platform recovery procedure rather than assuming no blob is needed.

If no Vera USB devices are found, the command exits with status 0 and prints a
JSON object with an `Error` field. USB subsystem initialization failure exits
non-zero.

## SetForceRecovery

`SetForceRecovery` drives board GPIOs so the CPU boots into USB RCM. It asserts
system reset, sets forced recovery, boot device select, and recovery type pins,
then releases reset. Each GPIO operation is executed through `gpiofind` and
`gpioset` with a 5 second timeout.

Use only the config type that matches the platform. The config controls the GPIO
names and the number of board instances affected. The config value is
case-insensitive.

Usage:

```bash
usbrcm-recovery-tool SetForceRecovery -c <config>
```

Config types:

| Value  | Platform                          | GPIO layout                    |
| ------ | --------------------------------- | ------------------------------ |
| `c2`   | E5010, E5020, Vera C2 MGX (P5035) | `B0_M1_CPU_*`, `B0_M1_*` reset |
| `c1g2` | PG558 Strata single board         | `BRD0_CPU_*`                   |
| `c2g4` | PG558 Strata dual board           | `BRD0_CPU_*` and `BRD1_CPU_*`  |

Examples:

```bash
usbrcm-recovery-tool SetForceRecovery -c c2
usbrcm-recovery-tool SetForceRecovery -c c1g2
usbrcm-recovery-tool SetForceRecovery -c c2g4
```

Example output:

```json
{
  "Board0": {
    "Status": "Successful"
  },
  "Status": "Successful"
}
```

For `c2g4`, output contains `Board0` and `Board1`. A failed operation sets the
top-level `"Status"` to `"Failed"` and exits non-zero.

## PerformUSBRecovery

`PerformUSBRecovery` opens the Vera device at the selected USB port path,
optionally sends a DOT blob first, then transfers each recovery image over the
RCM bulk OUT endpoint. The tool checks progress codes between image transfers.
It does not force the CPU into USB RCM; use `GetRecoveryStatus` and
`SetForceRecovery` first when needed.

Usage:

```bash
usbrcm-recovery-tool PerformUSBRecovery \
  -p <port-path> \
  [-b <dot-blob>] \
  -i <image1> [<image2> ...] \
  [-v|--verbose]
```

Options:

- `-p, --port-path <path>`: Required. USB port path from `GetRecoveryStatus`,
  for example `1-1.3`.
- `-b, --blob <path>`: Optional DOT blob. Required when `DOT Blob Required` is
  `Yes`.
- `-i, --images <files...>`: Required. Recovery image files in transfer order.
- `-v, --verbose`: Print diagnostic transfer logging to stdout and stderr. Do
  not use for machine-parsed JSON output.

Example output on success:

```json
{
  "Status": "Successful"
}
```

On some DOT flows, success may include:

```json
{
  "Status": "Successful",
  "EmptyDotBlobAccepted": true
}
```

Failures set `"Status": "Failed"` and include an `"Error"` string and numeric
`"ErrorCode"`. If ECID indicates that an S2A blob is required, the standalone
tool reports that S2A blob recovery is not supported.

## Troubleshooting

- `No USB devices found with Vera VID:PID=0x0955:0x7410`: Confirm the CPU is in
  USB RCM, the USB path is connected to the BMC, and `SetForceRecovery` was run
  with the right config.
- `Failed to open USB device` or `Failed to claim USB interface`: Run with
  sufficient privileges and check that no other process is using the USB RCM
  interface.
- `"DOT blob required but not provided"`: Re-run with `-b /path/to/dot_blob.bin`
  before `-i`.
- `"S2A blob required - not supported"`: The device requires an S2A flow that
  this standalone CLI does not support. Follow the platform recovery procedure.
- `"Failed to read image file"`: Check that every image path exists on the BMC
  and is readable.
- `SetForceRecovery` reports `"Failed"`: Confirm `gpiofind` and `gpioset` are
  installed, the GPIO names match the selected config, and no other service is
  holding the GPIO lines.

Use `-v` only when collecting diagnostic logs:

```bash
usbrcm-recovery-tool GetRecoveryStatus -v
usbrcm-recovery-tool PerformUSBRecovery -p 1-1.3 -i /path/to/image.bin -v
```
