# NVSwitch OOB Configuration Manager

Standalone D-Bus service (`com.Nvidia.Software.NVSwitchConfig.Updater`) that
manages out-of-band NVLink boot configuration delivery to NVSwitch QM devices on
HGX platforms.

Binary: `/usr/bin/nvswitch-config-manager`

---

## Overview

The service is split into two cooperating phases that run inside a single
`boost::asio` event loop — no worker threads or mutexes are required.

### Phase 1 — Config file management

Exposes two D-Bus methods on object path `/com/nvidia/NVSwitchConfig` under
interface `com.nvidia.SwitchConfig.Updater` so that bmcweb (Redfish back-end)
can persist a YAML NVLink configuration file to eMMC storage.

| Method             | In signature | Description                                                  |
| ------------------ | :----------: | ------------------------------------------------------------ |
| `AddConfigFile`    |     `h`      | Validate and store fd content to eMMC. Fails if file exists. |
| `RemoveConfigFile` |   _(none)_   | Delete the stored config file. Fails if no file is present.  |

The file is validated against the embedded `metadata` section (blob counts and
per-blob byte sizes) before being written. The write is atomic: content is first
written to a `.tmp` sibling file, then renamed into place.

Storage location: `/var/emmc/misc/switch-config/nvswitch-nvlink-config.yaml`
(directory is created automatically if absent).

### Phase 2 — Configuration push on device ready

At startup the service queries ObjectMapper (`GetSubTree`) for all objects
implementing `com.nvidia.DeviceConfiguration.DeviceConfigRequestEvent` and
subscribes to the `DeviceConfigurationRequested` signal on each discovered path.
New devices appearing later are picked up via an `InterfacesAdded` watcher — no
restart required.

On signal receipt:

1. The stored YAML config is read and parsed into an ordered blob list:
   `ports_down[0..N]` → `configure[0..N]` → `ports_up[0..N]`.
2. Each blob is packed into an anonymous `memfd` and sent to the device via an
   async `SetDeviceConfiguration` call on
   `com.nvidia.DeviceConfiguration.DeviceConfigAsync`.
3. The returned async-operation object path is polled (200 ms interval, 30 s
   timeout) on `com.nvidia.Async.Status` until the operation completes or times
   out.
4. Blobs are sent strictly in sequence; a failure on one blob is logged and
   skipped without aborting the remaining blobs.
5. A final summary log records the success/failure counts.

Duplicate signals for the same device while a push is already in progress are
silently dropped.

---

## D-Bus details

```text
Service:   com.Nvidia.Software.NVSwitchConfig.Updater
Object:    /com/nvidia/NVSwitchConfig
Interface: com.nvidia.SwitchConfig.Updater

Methods:
  AddConfigFile    (h) -> ()
  RemoveConfigFile ()  -> ()
```

Interfaces consumed:

| Interface                                                 | Method / Signal                         | Purpose                        |
| --------------------------------------------------------- | --------------------------------------- | ------------------------------ |
| `xyz.openbmc_project.ObjectMapper`                        | `GetSubTree`, `GetObject`               | Device and service discovery   |
| `org.freedesktop.DBus.ObjectManager`                      | `InterfacesAdded`                       | Hot-plug device tracking       |
| `com.nvidia.DeviceConfiguration.DeviceConfigRequestEvent` | `DeviceConfigurationRequested` (signal) | Trigger config push per device |
| `com.nvidia.DeviceConfiguration.DeviceConfigAsync`        | `SetDeviceConfiguration`                | Push one blob to a device      |
| `com.nvidia.Async.Status`                                 | `Status` property                       | Poll async operation result    |

---

## Config file format

The config file is a YAML document. Both underscored (`ports_down`) and
hyphenated (`ports-down`) key names are accepted.

```yaml
ports_down:
  - <hex-encoded bytes, e.g. deadbeef01020304>
  - <hex-encoded bytes>
configure:
  - <hex-encoded bytes>
ports_up:
  - <hex-encoded bytes>
  - <hex-encoded bytes>
metadata:
  ports_down_count: 2
  ports_down_sizes:
    - 8
    - 8
  configure_count: 1
  configure_sizes:
    - 4
  ports_up_count: 2
  ports_up_sizes:
    - 8
    - 8
```

Rules:

- Each list item under `ports_down`, `configure`, and `ports_up` is a continuous
  lowercase or uppercase hex string representing one blob.
- `metadata` counts must match the number of blobs parsed in each section.
- `metadata` sizes (bytes) must match the decoded byte length of each blob.
- Maximum file size: 64 KB.

---

## Build

Enable the feature via the Meson option:

```text
-DNVSWITCH_CONFIG_SUPPORT=enabled
```

Dependencies pulled in automatically: `sdbusplus`, `phosphor-logging`, `boost`.

---

## Testing

### Verify the service is running

```bash
systemctl status com.Nvidia.Software.NVSwitchConfig.Updater
```

### Inspect the D-Bus tree

```bash
busctl tree com.Nvidia.Software.NVSwitchConfig.Updater
busctl introspect com.Nvidia.Software.NVSwitchConfig.Updater /com/nvidia/NVSwitchConfig
```

### Follow live logs

```bash
journalctl -f -u com.Nvidia.Software.NVSwitchConfig.Updater
```

Expected output after a successful config push to one device:

```text
nvswitch-config-manager: DeviceConfigurationRequested signal received from /xyz/openbmc_project/...NVSwitch_0
nvswitch-config-manager: applyConfigToDevice: ports_down[0] success on .../NVSwitch_0
nvswitch-config-manager: applyConfigToDevice: configure[0] success on .../NVSwitch_0
nvswitch-config-manager: applyConfigToDevice: ports_up[0] success on .../NVSwitch_0
nvswitch-config-manager: applyConfigToDevice: COMPLETE for .../NVSwitch_0 – 3/3 blobs OK, 0 failure(s)
```

---

## Key implementation notes

- **Single-threaded async**: all D-Bus calls use `async_method_call`; async-op
  polling uses `boost::asio::steady_timer`. No blocking calls on the event loop.
- **Dynamic device discovery**: devices are found via ObjectMapper at startup
  and via `InterfacesAdded` for devices that enumerate later. Per-device state
  is keyed by object path.
- **memfd blob transfer**: each blob is written to an anonymous in-kernel
  `memfd` (`memfd_create(MFD_CLOEXEC)`) and passed as a unix fd (`h`) over D-Bus
  to avoid large message payloads.
- **Atomic file write**: config content is written to a `.tmp` file then
  `rename`-d to the final path, preventing partial reads by a concurrent push.
- **Custom YAML parser**: no third-party YAML library is used. The parser
  handles the specific two-level structure described above using a simple
  line-by-line state machine.
