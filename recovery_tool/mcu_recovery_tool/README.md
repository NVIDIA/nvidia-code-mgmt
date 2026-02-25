# MCU Recovery Tool

Firmware recovery and management for Microcontroller Unit (MCU) devices on
NVIDIA platforms. Use it to recover or reset MCUs over **USB** or **I2C**, enter
recovery mode via GPIO, and flash SB3/SB4 images using NXP's **blhost**.

---

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Executables](#executables)
- [Configuration Sources](#configuration-sources)
- [Entity Manager configuration](#entity-manager-configuration)
- [mcu-recovery (simple app)](#mcu-recovery-simple-app)
- [Systemd integration](#systemd-integration)
- [mcu-recovery-tool (CLI)](#mcu-recovery-tool-cli)
- [Recovery Flow](#recovery-flow)
- [Exit Codes and Errors](#exit-codes-and-errors)
- [Troubleshooting](#troubleshooting)

---

## Overview

- **Purpose:** Recover MCU devices that are in a bad state (e.g., in
  recovery/ISP mode, wrong firmware, or unresponsive) by flashing an SB3/SB4
  firmware image.
- **Interfaces:** USB (libusb) and I2C.
- **Configuration:** Either from D-Bus (Entity Manager) or from a JSON file.
- **Dependencies:** `blhost` (NXP bootloader host tool), GPIO access for reset
  and recovery pins, and (when using D-Bus) Entity Manager with
  `xyz.openbmc_project.Configuration.MCURecovery` objects.

---

## Prerequisites

1. **blhost** NXP's bootloader host utility must be installed and on `PATH`. The
   tool runs commands such as:
   - `blhost ... get-property security-state`
   - `blhost ... read-memory 0x1004160 48`
   - `blhost ... receive-sb-file <path>`

2. **Entity Manager (when using D-Bus)**  
   MCU configuration is read from D-Bus interface:
   - **Interface:** `xyz.openbmc_project.Configuration.MCURecovery`
   - **Service:** `xyz.openbmc_project.EntityManager`
   - **Object path:** Under `/xyz/openbmc_project/inventory/...`

   Each MCU configuration object can include:
   - **Target** – Target type (e.g. `CX9`, `HPM`).
   - **Interface** – `USB` or `I2C`.
   - **ResetGpioName**, **RecoveryGpioName** – GPIO line names for reset and
     recovery.
   - **USB:** **USBPort** (e.g. `1-1.2.1`), **ProductId** (functional PID).
   - **I2C:** **I2CBus**, **NormalI2CAddress**, **RecoveryI2CAddress**.

3. **Permissions**
   - Access to USB devices (e.g. udev rules or root).
   - Access to GPIO (e.g. group or root).
   - Access to I2C devices (e.g. `/dev/i2c-*`) when using I2C MCUs.

4. **Firmware image**
   - Valid SB3 or SB4 file (magic and version checked by the tool).

---

## Executables

| Executable            | Role                                                                                                                         |
| --------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **mcu-recovery**      | Simple, script-friendly entry: `mcu-recovery <BINARY> <TARGET>`. Uses D-Bus by target only; no subcommands.                  |
| **mcu-recovery-tool** | Full CLI with subcommands: PerformRecovery, ForceReset, GetDeviceStatus, SetForceRecovery. Supports JSON, D-Bus, and target. |

---

## Configuration Sources

You must specify **exactly one** of:

- **JSON file** (`-j` / `--json`) – MCU list and parameters from a file (see
  JSON format below).
- **Entity Manager on D-Bus** (`-e` / `--entity-manager`) – Configuration from
  D-Bus. You then scope by:
  - **Target** (`-t` / `--target`) – e.g. `CX9`, `HPM` (all MCUs with that
    Target).
  - **All** – Omit target when the subcommand allows it (e.g. ForceReset,
    GetDeviceStatus, SetForceRecovery).

JSON format (for `parseJsonFile` / `-j`): array under `"Exposes"` with objects
of `"Type": "MCURecovery"` and fields such as: `Name`, `Interface`,
`ResetGpioName`, `RecoveryGpioName`, and for USB: `USBPort`, `ProductId`; for
I2C: `I2CBus`, `NormalI2CAddress`, `RecoveryI2CAddress`.

---

## Entity Manager configuration

When using D-Bus (`-e` / `--entity-manager`), the tool discovers MCUs by calling
**GetSubTree** on the Object Mapper for paths under
`/xyz/openbmc_project/inventory` that implement the interface below. Each such
object describes one MCU.

### Interface and service

| Item            | Value                                                                                                                |
| --------------- | -------------------------------------------------------------------------------------------------------------------- |
| **Service**     | `xyz.openbmc_project.EntityManager`                                                                                  |
| **Interface**   | `xyz.openbmc_project.Configuration.MCURecovery`                                                                      |
| **Object path** | Any under `/xyz/openbmc_project/inventory/...` (e.g. `/xyz/openbmc_project/inventory/system/chassis/IO_Board_SMA_0`) |

The device name used by the tool (e.g. in logs and as the config key) is the
**last path component** of the object path (e.g. `IO_Board_SMA_0`).

### Properties

All properties are on the interface
`xyz.openbmc_project.Configuration.MCURecovery`.

#### Common (all MCUs)

| Property             | Type   | Required              | Description                                                                            |
| -------------------- | ------ | --------------------- | -------------------------------------------------------------------------------------- |
| **Interface**        | string | No (default: `"USB"`) | Transport: `"USB"` or `"I2C"`.                                                         |
| **ResetGpioName**    | string | **Yes**               | GPIO line name (e.g. device-tree name) for the MCU reset pin.                          |
| **RecoveryGpioName** | string | **Yes**               | GPIO line name for the recovery/ISP mode pin.                                          |
| **Target**           | string | No                    | Target type used by `-t` / `--target` (e.g. `CX9`, `HPM`). Omit only if you use "all". |

#### USB MCUs only

| Property      | Type   | Required | Description                                                                         |
| ------------- | ------ | -------- | ----------------------------------------------------------------------------------- |
| **USBPort**   | string | **Yes**  | USB port path (e.g. `1-1.2.1`) where the MCU appears.                               |
| **ProductId** | uint64 | **Yes**  | Functional (normal mode) USB product ID (0–65535). Used to detect a healthy device. |

#### I2C MCUs only

| Property               | Type   | Required | Description                                 |
| ---------------------- | ------ | -------- | ------------------------------------------- |
| **I2CBus**             | uint64 | **Yes**  | I2C bus number (0–255).                     |
| **NormalI2CAddress**   | uint64 | **Yes**  | I2C address in normal operation (0–65535).  |
| **RecoveryI2CAddress** | uint64 | **Yes**  | I2C address in recovery/ISP mode (0–65535). |

### Example Entity Manager YAML

Typical configuration is in YAML files that Entity Manager loads (e.g. under
`/usr/share/entity-manager/` or a platform-specific overlay). Each MCU is an
object that implements `xyz.openbmc_project.Configuration.MCURecovery`:

**USB MCU example:**

```yaml
- name: IO_Board_SMA_0
  Type: entity
  Class: MCURecovery
  xyz.openbmc_project.Configuration.MCURecovery:
    Target: "CX9"
    Interface: "USB"
    ResetGpioName: "MCU_RESET_N"
    RecoveryGpioName: "MCU_RECOVERY_N"
    USBPort: "1-1.2.1"
    ProductId: 0x1234
```

**I2C MCU example:**

```yaml
- name: HPM_0
  Type: entity
  Class: MCURecovery
  xyz.openbmc_project.Configuration.MCURecovery:
    Target: "HPM"
    Interface: "I2C"
    ResetGpioName: "HPM_RESET_N"
    RecoveryGpioName: "HPM_RECOVERY_N"
    I2CBus: 4
    NormalI2CAddress: 0x50
    RecoveryI2CAddress: 0x51
```

Ensure object paths and `name`/`Class` match how your Entity Manager layout and
merge rules expose the MCURecovery interface under
`/xyz/openbmc_project/inventory/...`.

---

## mcu-recovery (simple app)

Used by automation or services; configuration comes only from D-Bus by
**target**.

**Usage:**

```bash
mcu-recovery <BINARY_PATH> <TARGET>
```

- **BINARY_PATH** – Path to the SB3/SB4 recovery image.
- **TARGET** – Target name (e.g. `CX9`, `HPM`) to select MCUs from Entity
  Manager.

**Example:**

```bash
mcu-recovery /usr/share/nvidia/fw/cx9_recovery.sb3 CX9
```

**Behavior:**

1. Reads MCU config from D-Bus for the given **TARGET**.
2. Initializes the recovery manager (including GPIO).
3. Runs the full recovery flow on all MCUs for that target (devices that are
   already healthy are skipped; unhealthy or recovery-mode devices are updated).
   The simple app has no force option; use **mcu-recovery-tool PerformRecovery
   -f** to force update.
4. Exits 0 on success, non-zero on error (e.g. no config, init failure, or
   recovery failure).

---

## Systemd integration

A template unit **`mcu-recovery@.service`** is provided for running recovery
from systemd or automation.

- **Unit:** `mcu-recovery@.service` — the instance (`%I`) is passed as `ARGS` to
  the service.
- **Executable:** Runs `/usr/bin/mcu-recovery $ARGS`.
- **Usage:** Pass the two arguments expected by the simple app as the instance:
  `<BINARY_PATH> <TARGET>`. Either use the instance string (escaping spaces as
  needed) or a drop-in that sets
  `EnvironmentFile=/etc/mcurecovery/mcurecovery.env` (or similar) so that `ARGS`
  is set to e.g. `"/usr/share/nvidia/fw/cx9_recovery.sb3 CX9"`.

Example:

```bash
# Instance as ARGS (escape or quote as required by systemctl):
systemctl start 'mcu-recovery@/usr/share/nvidia/fw/cx9_recovery.sb3 CX9.service'

# Or use a drop-in that sets EnvironmentFile and ARGS:
# ARGS="/usr/share/nvidia/fw/cx9_recovery.sb3 CX9"
systemctl start mcu-recovery@instance
```

On Yocto builds, `mcurecovery.env` is often installed by a recipe and wired into
related services (e.g. `com.Nvidia.MCURecovery.Starter.service`) via
`EnvironmentFile=`.

---

## mcu-recovery-tool (CLI)

Full-featured CLI. You must choose exactly one of: **JSON** or **Entity
Manager**; with Entity Manager you can optionally use **target**.

**General:**

```bash
mcu-recovery-tool <SUBCOMMAND> [OPTIONS]
```

**Subcommands:**

- **PerformRecovery** – Run recovery (flash SB3/SB4). Requires `--target` or
  `--json` (no "all devices").
- **ForceReset** – Reset MCUs via GPIO until they report healthy (up to 10
  attempts). Allows "all" when using D-Bus (no target).
- **GetDeviceStatus** – Refresh device info and print status (healthy / recovery
  / unknown). Allows "all" when using D-Bus.
- **SetForceRecovery** – Put MCUs into recovery mode (GPIO sequence), then
  verify. Allows "all" when using D-Bus.

---

### PerformRecovery

Run recovery for MCUs matching the given config (and optionally force update
healthy devices).

**Options:**

| Option             | Short | Description                                                   |
| ------------------ | ----- | ------------------------------------------------------------- |
| `--image`          | `-i`  | **Required.** Path to SB3/SB4 recovery image.                 |
| `--force`          | `-f`  | (Flag) Force recovery even if the device is already healthy.  |
| `--json`           | `-j`  | JSON config file (overrides target).                          |
| `--entity-manager` | `-e`  | Use Entity Manager on D-Bus.                                  |
| `--target`         | `-t`  | Target (e.g. CX9, HPM). Required with `-e` if not using `-j`. |

**Examples:**

```bash
# By target (D-Bus)
mcu-recovery-tool PerformRecovery -e -t HPM -i /path/to/hpm_recovery.sb3

# Force update even if healthy
mcu-recovery-tool PerformRecovery -e -t CX9 -i /path/to/cx9.sb3 -f

# Using JSON config
mcu-recovery-tool PerformRecovery -j /etc/mcu_config.json -i /path/to/image.sb3
```

---

### ForceReset

Reset MCUs via GPIO and retry until they become healthy (or up to 10 attempts).
Each attempt drives the reset pin low for 500 ms, then high, then waits 3 s
before re-probing device health. MCUs that become healthy are removed from the
list and their GPIO lines released; the loop continues until all are healthy or
10 attempts are reached.

**Options:** Same configuration options as above (`-j`, `-e`, `-t`). With `-e`
and no `-t`, all MCUs from Entity Manager are used.

**Examples:**

```bash
mcu-recovery-tool ForceReset -e -t HPM
mcu-recovery-tool ForceReset -e
mcu-recovery-tool ForceReset -j /etc/mcu_config.json
```

---

### GetDeviceStatus

Refresh device info from USB/I2C and print whether each MCU is healthy, in
recovery mode, or in an unknown state.

**Options:** Same as above. With `-e` and no `-t`, all MCUs are queried.

**Examples:**

```bash
mcu-recovery-tool GetDeviceStatus -e -t CX9
mcu-recovery-tool GetDeviceStatus -e
mcu-recovery-tool GetDeviceStatus -j /etc/mcu_config.json
```

---

### SetForceRecovery

Put selected MCUs into recovery mode (recovery pin low, then reset toggle, 3 s
wait), wait for USB re-enumeration (polling up to 3×1 s), then verify each
device is in recovery mode. Exits with an error if any device fails
verification.

**Options:** Same configuration options. With `-e` and no `-t`, all MCUs are
forced into recovery.

**Examples:**

```bash
mcu-recovery-tool SetForceRecovery -e -t HPM
mcu-recovery-tool SetForceRecovery -e
mcu-recovery-tool SetForceRecovery -j /etc/mcu_config.json
```

---

## Recovery Flow

When **PerformRecovery** (or the simple **mcu-recovery** app) runs:

1. **Config** – MCU list is obtained (D-Bus by target or JSON).
2. **Init** – libusb (if any USB MCUs), GPIO lines (reset/recovery) are
   initialized.
3. **Device info** – Each MCU is probed (USB port path or I2C address).
4. **Health** – For each device:
   - If **healthy** (USB: functional PID matches or device has MCTP class; I2C:
     responds at normal address): skip recovery unless **force** is set. For
     some USB MCUs (e.g. Iris/PX86E), health is determined by MCTP class because
     the product ID can be the same in normal and recovery mode.
   - If **not healthy** (e.g. in recovery/ISP or unknown): continue to recovery.
5. **Enter recovery** – If recovery is needed (or forced): recovery pin low,
   reset pin low→high (500 ms active, then 3 s delay).
6. **Provisioning check** – Non-provisioned (e.g. wrong vendor ID) devices are
   skipped with an error.
7. **blhost** – Security state, optional encrypt-key check, then
   `receive-sb-file <image>`.
8. **Exit recovery** – Recovery pin high, reset toggle; wait for device to come
   back.
9. **Verify** – Re-probe; success if device is healthy again.
10. **Message registry** – On success/failure, Redfish-style events may be
    written (when a message registry is used, e.g. in the simple app).

SB3/SB4 file is validated before use: SB3 by magic/version in the first 8 bytes;
SB4 by header bytes (e.g. first byte 0x02, fourth 0x87).

---

## Exit Codes and Errors

- **0** – Success.
- **Non-zero** – Failure (invalid arguments, no config, init failure, recovery
  failure, or exception).

Common error paths:

- **Invalid arguments** – e.g. `mcu-recovery` with fewer than 2 arguments
  (BINARY and TARGET are required), or missing `-i` for PerformRecovery.
- **No MCU config** – No devices for the given target or empty JSON.
- **Init failure** – libusb or GPIO init failed.
- **Device not found** – USB port or I2C address not present after retries.
- **Recovery failed** – blhost command failed, or device not healthy after
  recovery.
- **Invalid SB file** – SB3/SB4 header check failed.
- **Security / key** – UNSECURE device and encrypt key not set, or read-memory
  failed.
- **Not provisioned** – Device skipped (e.g. wrong vendor ID).

When the tool is used with a message registry (e.g. **mcu-recovery**), errors
are also reported via the registry (e.g. `deviceRecoveryFailed`,
`noDevicesFound`, `InvalidSBFile`).

---

## Troubleshooting

| Symptom                                                   | What to check                                                                                                               |
| --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| "Invalid number of arguments" (mcu-recovery)              | Provide both arguments: `mcu-recovery <BINARY> <TARGET>`.                                                                   |
| "Specify exactly one of -j/--json or -e/--entity-manager" | Use either `-j <file>` or `-e` (and optionally `-t`), not both.                                                             |
| "No MCU configuration found"                              | Target name in Entity Manager, or JSON path and content.                                                                    |
| "Failed to initialize libusb"                             | USB access (udev, permissions), and that at least one MCU is USB.                                                           |
| "GPIO line not found" / "Failed to open GPIO"             | ResetGpioName/RecoveryGpioName in config and GPIO chip/line naming (e.g. libgpiod).                                         |
| "Device not found on USB port"                            | USB topology (port path), cable, and that the MCU is powered and enumerated. The tool retries up to 5 times with 2 s delay. |
| "Get security state failed" / "Write flash failed"        | blhost on PATH; device in recovery mode; correct USB bus:device and VID:PID for blhost.                                     |
| "Encrypt key is not set"                                  | Device is UNSECURE and encrypt key is required for receive-sb-file; key must be set or use a different image flow.          |
| "Recovery failed, PID does not match"                     | Device came back with wrong product ID; possible wrong image or device type.                                                |
| I2C "not found" / "Failed to set I2C address"             | I2C bus number, normal/recovery addresses, and `/dev/i2c-*` permissions.                                                    |

**Recommendations:**

- Use **GetDeviceStatus** first to see which MCUs are healthy or in recovery.
- Use **SetForceRecovery** to force all (or selected) MCUs into recovery before
  external use of blhost or other tools.
- Ensure **blhost** version matches the MCU bootloader expectations.
- Confirm Entity Manager has the correct **Target** and MCURecovery properties
  for your platform.

---

## Build and Install

From the **code-mgmt** project root (e.g. `nvidia-code-mgmt/`):

```bash
meson setup build && ninja -C build
# Executables: build/recovery_tool/mcu_recovery_tool/mcu-recovery-tool, build/recovery_tool/mcu_recovery_tool/mcu-recovery
# Install: ninja -C build install  (to configured bindir, typically /usr/bin)
```

**Dependencies** (see `recovery_tool/mcu_recovery_tool/meson.build`):
libusb-1.0, nlohmann_json, libgpiocxx (gpiod), sdbusplus,
phosphor-dbus-interfaces, phosphor-logging, and project-local `common`
(message_registry), `src` (dbusutils), and recovery_tool `common` as needed.
