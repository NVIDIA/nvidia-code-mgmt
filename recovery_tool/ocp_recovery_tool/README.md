# OCP Recovery Tool

Command-line tool for performing OCP (Open Compute Project) recovery operations
on devices over I2C. Runs on the BMC and communicates with OCP-compliant device
recovery endpoints.

## Common Options

All subcommands accept the following options:

| Option               | Description                                                        |
| -------------------- | ------------------------------------------------------------------ |
| `-b, --bus <N>`      | I2C bus address (mutually exclusive with `-p`)                     |
| `-p, --port <path>`  | USB port path for USB-to-I2C bridge (mutually exclusive with `-b`) |
| `-s, --slave <addr>` | I2C slave address of the device (**required**)                     |
| `-v, --verbose`      | Enable verbose output (raw I2C TX/RX data)                         |
| `-e, --emulation`    | Enable emulation mode for testing                                  |

Either `--bus` or `--port` must be provided, but not both.

## Commands

### GetDeviceID

Retrieves the device identification information including vendor ID, device ID
type, and component identifiers.

```bash
ocp-recovery-tool GetDeviceID -b 70 -s 0x69
```

Example output:

```json
{
  "Component Identifier": "0x1234",
  "Device ID Type": "PCI Vendor",
  "Device Identifier": "0x10de",
  "Vendor String": "Vendor"
}
```

### GetDeviceStatus

Retrieves the current operational status of the device, including whether it is
in normal operation, recovery mode, or an unknown state.

```bash
ocp-recovery-tool GetDeviceStatus -b 70 -s 0x69
```

Example output:

```json
{
  "Device Status": "Recovery Mode",
  "Protocol Error": "No Error",
  "Recovery Reason": "Missing/corrupt key manifest",
  "Vendor Status(in hex)": "0x00"
}
```

### SetForceRecovery

Forces the device into recovery mode. This must be done before performing OCP
recovery if the device is not already in recovery mode.

```bash
ocp-recovery-tool SetForceRecovery -b 70 -s 0x69
```

Example output:

```json
{
  "Status": "Success"
}
```

### GetRecoveryStatus

Retrieves the current recovery status, indicating whether recovery is in
progress, successful, or failed.

```bash
ocp-recovery-tool GetRecoveryStatus -b 70 -s 0x69
```

Example output:

```json
{
  "Device Recovery Status": "Recovery successful",
  "Recovery Image Index": "3",
  "Vendor Specific Status": "0"
}
```

### PerformOCPRecovery

Writes one or more firmware images to the device over the OCP recovery
interface. Images are written sequentially to CMS0, each followed by an
activation step.

```bash
ocp-recovery-tool PerformOCPRecovery -b 70 -s 0x69 \
    -i /path/to/image0.bin \
       /path/to/image1.bin \
       /path/to/image2.bin \
       /path/to/image3.bin
```

| Option                    | Description                                |
| ------------------------- | ------------------------------------------ |
| `-i, --images <paths...>` | List of firmware image file paths to write |

Example output:

```text
Initiating recovery image write process...
Writing Image (/path/to/image0.bin to CMS0), Progress: 10% (16256 / 162304 bytes)
Writing Image (/path/to/image0.bin to CMS0), Progress: 20% (32480 / 162304 bytes)
...
Writing Image (/path/to/image0.bin to CMS0), Progress: 100% (162304 / 162304 bytes)
```

### GetCMSLogs

Retrieves CMS (Component Measurement and Status) logs from the device and saves
them to a file.

```bash
ocp-recovery-tool GetCMSLogs -b 70 -s 0x69 -w 2
```

| Option                 | Description                                     |
| ---------------------- | ----------------------------------------------- |
| `-w, --window <N>`     | CMS window to retrieve logs from (**required**) |
| `-o, --outfile <path>` | Output file path (default: `/var/cms2_log.bin`) |

Example output:

```json
{
  "Status": "Success",
  "LogFile": "/var/cms2_log.bin"
}
```

## Using USB Port Instead of Bus Address

If the device is connected through a USB-to-I2C bridge, use the `-p` flag with
the USB port path instead of `-b`:

```bash
ocp-recovery-tool GetDeviceStatus -p usb1_1_1_1 -s 0x69
```

## Full Recovery Workflow

A typical manual recovery flow:

```bash
# 1. Check device status
ocp-recovery-tool GetDeviceStatus -b <bus> -s <slave_addr>

# 2. Force device into recovery mode (if not already)
ocp-recovery-tool SetForceRecovery -b <bus> -s <slave_addr>

# 3. Perform recovery with firmware images
ocp-recovery-tool PerformOCPRecovery -b <bus> -s <slave_addr> \
    -i /path/to/image0.bin /path/to/image1.bin \
       /path/to/image2.bin /path/to/image3.bin

# 4. Verify recovery completed successfully
ocp-recovery-tool GetRecoveryStatus -b <bus> -s <slave_addr>
```
