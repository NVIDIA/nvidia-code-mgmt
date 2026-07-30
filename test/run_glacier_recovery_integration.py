#!/usr/bin/env python3

import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ENTITY_MANAGER = "xyz.openbmc_project.EntityManager"
ROOT_PATH = "/xyz/openbmc_project/inventory"
GLACIER_IFACE = "xyz.openbmc_project.Configuration.GlacierCrisisRecovery"
GPIO_IFACE = "xyz.openbmc_project.Configuration.GPIOERoTRecovery"


def build_image(path: Path) -> None:
    image = bytearray((i * 3) & 0xFF for i in range(3000))
    image[0] = 1
    image[1] = 0
    image[2] = 0
    img_offset = 256
    blob_addr = 2048
    image[img_offset : img_offset + 4] = b"PHCM"
    image[img_offset + 4 : img_offset + 8] = (1).to_bytes(4, "little")
    image[img_offset + 8 : img_offset + 12] = (0x10).to_bytes(4, "little")
    image[img_offset + 12 : img_offset + 16] = (0x20).to_bytes(4, "little")
    image[img_offset + 16 : img_offset + 20] = (1).to_bytes(4, "little")
    image[img_offset + 0x2C : img_offset + 0x30] = blob_addr.to_bytes(
        4, "little"
    )
    path.write_bytes(image)


def scenario_objects(name: str):
    if name == "empty":
        return {}

    return {
        f"{ROOT_PATH}/dev1": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(True),
                "I2CBus": dbus.UInt64(1),
                "I2CAddress": dbus.UInt64(0x21),
            }
        },
        f"{ROOT_PATH}/dev2": {
            GPIO_IFACE: {
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(2),
                "I2CAddress": dbus.UInt64(0x22),
            }
        },
        f"{ROOT_PATH}/dev3": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(3),
                "I2CAddress": dbus.UInt64(0x23),
            }
        },
        f"{ROOT_PATH}/dev4": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(4),
                "I2CAddress": dbus.UInt64(0x24),
            }
        },
        f"{ROOT_PATH}/dev5": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(5),
                "I2CAddress": dbus.UInt64(0x25),
            }
        },
        f"{ROOT_PATH}/dev6": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(6),
                "I2CAddress": dbus.UInt64(0x26),
            }
        },
        f"{ROOT_PATH}/dev7": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(7),
                "I2CAddress": dbus.UInt64(0x27),
            }
        },
        f"{ROOT_PATH}/dev8": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(True),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(8),
                "I2CAddress": dbus.UInt64(0x28),
            }
        },
        f"{ROOT_PATH}/skip": {
            GLACIER_IFACE: {
                "isRecoverable": dbus.Boolean(False),
                "HiddenByFPGA": dbus.Boolean(False),
                "I2CBus": dbus.UInt64(9),
                "I2CAddress": dbus.UInt64(0x29),
            }
        },
    }


class InventoryRoot(dbus.service.Object):
    def __init__(self, bus, objects):
        super().__init__(bus, ROOT_PATH)
        self.objects = objects

    @dbus.service.method(
        "org.freedesktop.DBus.ObjectManager", out_signature="a{oa{sa{sv}}}"
    )
    def GetManagedObjects(self):
        return self.objects


class InventoryObject(dbus.service.Object):
    def __init__(self, bus, path, interfaces):
        super().__init__(bus, path)
        self.interfaces = interfaces

    @dbus.service.method(
        "org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v"
    )
    def Get(self, interface_name, property_name):
        if interface_name not in self.interfaces:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.UnknownInterface"
            )
        if property_name not in self.interfaces[interface_name]:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.UnknownProperty"
            )
        return self.interfaces[interface_name][property_name]


def serve(address: str, name: str) -> int:
    DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(address)
    bus_name = dbus.service.BusName(ENTITY_MANAGER, bus)
    objects = scenario_objects(name)
    root = InventoryRoot(bus, objects)
    inventory_objects = [
        InventoryObject(bus, path, interfaces)
        for path, interfaces in objects.items()
    ]
    loop = GLib.MainLoop()

    def _term(*_args):
        loop.quit()

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)
    loop.run()
    # Keep references alive until after the loop exits.
    del bus_name
    del inventory_objects
    del root
    return 0


def start_bus():
    proc = subprocess.run(
        [
            "dbus-daemon",
            "--session",
            "--fork",
            "--print-address",
            "1",
            "--print-pid",
            "1",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    address, pid = proc.stdout.strip().splitlines()
    return address, int(pid)


def linked_library_path(binary: str, library_name: str) -> str:
    result = subprocess.run(
        ["ldd", binary], check=False, capture_output=True, text=True
    )
    if result.returncode != 0:
        return ""

    for line in result.stdout.splitlines():
        if library_name not in line:
            continue
        if "=>" in line:
            candidate = line.split("=>", 1)[1].split("(", 1)[0].strip()
        else:
            candidate = line.strip().split(" ", 1)[0]
        if candidate and candidate != "not found":
            return candidate
    return ""


def build_ld_preload(binary: str, preload: str, existing: str | None) -> str:
    entries = []
    asan = linked_library_path(binary, "libasan.so")
    if asan:
        entries.append(asan)
    entries.append(preload)
    if existing:
        entries.append(existing)
    return ":".join(entries)


def run_binary(
    binary: str,
    preload: str,
    scenario: str,
    image: Path | None,
    expect_nonzero: bool,
) -> None:
    address, bus_pid = start_bus()
    env = os.environ.copy()
    env["DBUS_SYSTEM_BUS_ADDRESS"] = address
    env["DBUS_SESSION_BUS_ADDRESS"] = address
    if preload:
        env["LD_PRELOAD"] = build_ld_preload(
            binary, preload, env.get("LD_PRELOAD")
        )

    service = subprocess.Popen(
        [sys.executable, __file__, "--serve", address, scenario],
        env=env,
    )
    try:
        time.sleep(0.5)
        cmd = [binary]
        if image is not None:
            cmd.append(str(image))
        result = subprocess.run(cmd, env=env, check=False)
        if result.returncode < 0:
            raise RuntimeError(
                f"{scenario} run terminated by signal {-result.returncode}"
            )
        if expect_nonzero and result.returncode == 0:
            raise RuntimeError(f"{scenario} run unexpectedly succeeded")
        if not expect_nonzero and result.returncode != 0:
            raise RuntimeError(
                f"{scenario} run unexpectedly failed: {result.returncode}"
            )
    finally:
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
            service.wait(timeout=5)
        os.kill(bus_pid, signal.SIGTERM)


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--serve":
        if len(sys.argv) < 4:
            print(
                "usage: run_glacier_recovery_integration.py --serve <address> <scenario>",
                file=sys.stderr,
            )
            return 2
        return serve(sys.argv[2], sys.argv[3])

    if len(sys.argv) != 3:
        print(
            "usage: run_glacier_recovery_integration.py <binary> <preload>",
            file=sys.stderr,
        )
        return 2

    binary = sys.argv[1]
    preload = sys.argv[2]

    with tempfile.TemporaryDirectory() as temp_dir:
        image = Path(temp_dir) / "glacier-image.bin"
        build_image(image)

        run_binary(binary, preload, "empty", image, expect_nonzero=True)
        run_binary(binary, preload, "mixed", image, expect_nonzero=True)

        result = subprocess.run([binary], check=False)
        if result.returncode < 0:
            raise RuntimeError(
                f"missing-arg run terminated by signal {-result.returncode}"
            )
        if result.returncode == 0:
            raise RuntimeError("missing-arg run unexpectedly succeeded")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
