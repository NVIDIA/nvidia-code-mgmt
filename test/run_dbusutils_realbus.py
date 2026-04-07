#!/usr/bin/env python3

import os
import signal
import subprocess
import sys
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

OBJECT_MAPPER = "xyz.openbmc_project.ObjectMapper"
MAPPER_PATH = "/xyz/openbmc_project/object_mapper"
MAPPER_IFACE = "xyz.openbmc_project.ObjectMapper"
ITEM_IFACE = "xyz.openbmc_project.Inventory.Item"
VERSION_IFACE = "xyz.openbmc_project.Software.Version"
INVENTORY_SERVICE = "xyz.openbmc_project.Inventory.Manager"
INVENTORY_PATH = "/xyz/openbmc_project/inventory/system/device0"
MODEL_MISMATCH_PATH = "/xyz/openbmc_project/inventory/system/model_mismatch"
MANUFACTURER_MISMATCH_PATH = (
    "/xyz/openbmc_project/inventory/system/manufacturer_mismatch"
)
EMPTY_MANUFACTURER_PATH = (
    "/xyz/openbmc_project/inventory/system/empty_manufacturer"
)
CHASSIS_SERVICE = "xyz.openbmc_project.State.Chassis"
CHASSIS_PATH = "/xyz/openbmc_project/state/chassis0"
CHASSIS_IFACE = "xyz.openbmc_project.State.Chassis"
ASSET_IFACE = "xyz.openbmc_project.Inventory.Decorator.Asset"
HOST_ON = "xyz.openbmc_project.State.Chassis.PowerState.On"
LOGGING_SERVICE = "xyz.openbmc_project.Logging"
LOGGING_PATH = "/xyz/openbmc_project/logging"
LOGGING_IFACE = "xyz.openbmc_project.Logging.Create"
SYSTEMD_SERVICE = "org.freedesktop.systemd1"
SYSTEMD_PATH = "/org/freedesktop/systemd1"
SYSTEMD_IFACE = "org.freedesktop.systemd1.Manager"
SOFTWARE_PATH = "/xyz/openbmc_project/software/other/test"
TEST_SERVICE = "xyz.openbmc_project.TestService"
MANAGER_PATH = "/test/manager"


class MapperObject(dbus.service.Object):
    @dbus.service.method(MAPPER_IFACE, in_signature="sias", out_signature="as")
    def GetSubTreePaths(self, path, depth, interfaces):
        if (
            path == "/xyz/openbmc_project/inventory/system"
            and ITEM_IFACE in interfaces
        ):
            return [INVENTORY_PATH]
        if (
            path == "/xyz/openbmc_project/software/other"
            and VERSION_IFACE in interfaces
        ):
            return [SOFTWARE_PATH]
        return []

    @dbus.service.method(
        MAPPER_IFACE, in_signature="sas", out_signature="a{sas}"
    )
    def GetObject(self, path, interfaces):
        inventory_paths = {
            INVENTORY_PATH,
            MODEL_MISMATCH_PATH,
            MANUFACTURER_MISMATCH_PATH,
            EMPTY_MANUFACTURER_PATH,
        }
        if path in inventory_paths and any(
            interface in interfaces for interface in [ITEM_IFACE, ASSET_IFACE]
        ):
            returned_interfaces = []
            if ITEM_IFACE in interfaces:
                returned_interfaces.append(ITEM_IFACE)
            if ASSET_IFACE in interfaces:
                returned_interfaces.append(ASSET_IFACE)
            return {INVENTORY_SERVICE: returned_interfaces}
        if path == CHASSIS_PATH and CHASSIS_IFACE in interfaces:
            return {CHASSIS_SERVICE: [CHASSIS_IFACE]}
        return {}


class InventoryObject(dbus.service.Object):
    def __init__(self, bus, path, manufacturer, model):
        super().__init__(bus, path)
        self.manufacturer = manufacturer
        self.model = model

    @dbus.service.method(
        "org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v"
    )
    def Get(self, interface_name, property_name):
        if interface_name != ASSET_IFACE:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.UnknownInterface"
            )
        if property_name == "Manufacturer":
            return dbus.String(self.manufacturer)
        if property_name == "Model":
            return dbus.String(self.model)
        raise dbus.exceptions.DBusException(
            "org.freedesktop.DBus.Error.UnknownProperty"
        )


class ChassisObject(dbus.service.Object):
    @dbus.service.method(
        "org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v"
    )
    def Get(self, interface_name, property_name):
        if (
            interface_name == CHASSIS_IFACE
            and property_name == "CurrentPowerState"
        ):
            return dbus.String(HOST_ON)
        raise dbus.exceptions.DBusException(
            "org.freedesktop.DBus.Error.UnknownProperty"
        )


class LoggingObject(dbus.service.Object):
    @dbus.service.method(
        LOGGING_IFACE, in_signature="ssa{ss}", out_signature="o"
    )
    def Create(self, message_id, severity, add_data):
        return dbus.ObjectPath("/xyz/openbmc_project/logging/entry/1")


class SystemdObject(dbus.service.Object):
    @dbus.service.method(SYSTEMD_IFACE, in_signature="ss", out_signature="o")
    def StartUnit(self, unit, mode):
        return dbus.ObjectPath("/org/freedesktop/systemd1/job/1")

    @dbus.service.method(SYSTEMD_IFACE, in_signature="ss", out_signature="o")
    def RestartUnit(self, unit, mode):
        return dbus.ObjectPath("/org/freedesktop/systemd1/job/2")


class ManagerObject(dbus.service.Object):
    @dbus.service.method(
        "org.freedesktop.DBus.ObjectManager", out_signature="a{oa{sa{sv}}}"
    )
    def GetManagedObjects(self):
        return {
            f"{MANAGER_PATH}/device0": {
                "test.Interface": {
                    "Enabled": dbus.Boolean(True),
                    "Name": dbus.String("device0"),
                }
            }
        }


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


def serve(address: str) -> int:
    DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(address)
    bus_names = [
        dbus.service.BusName(name, bus)
        for name in [
            OBJECT_MAPPER,
            INVENTORY_SERVICE,
            CHASSIS_SERVICE,
            LOGGING_SERVICE,
            SYSTEMD_SERVICE,
            TEST_SERVICE,
        ]
    ]
    objects = [
        MapperObject(bus, MAPPER_PATH),
        InventoryObject(bus, INVENTORY_PATH, "NVIDIA", "Model-A"),
        InventoryObject(bus, MODEL_MISMATCH_PATH, "NVIDIA", "OtherModel"),
        InventoryObject(bus, MANUFACTURER_MISMATCH_PATH, "ACME", "Model-A"),
        InventoryObject(bus, EMPTY_MANUFACTURER_PATH, "", "Model-A"),
        ChassisObject(bus, CHASSIS_PATH),
        LoggingObject(bus, LOGGING_PATH),
        SystemdObject(bus, SYSTEMD_PATH),
        ManagerObject(bus, MANAGER_PATH),
    ]
    loop = GLib.MainLoop()

    def _term(*_args):
        loop.quit()

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)
    loop.run()
    del objects
    del bus_names
    return 0


def run_binary(binary: str) -> int:
    address, bus_pid = start_bus()
    env = os.environ.copy()
    env["DBUS_SYSTEM_BUS_ADDRESS"] = address
    env["DBUS_SESSION_BUS_ADDRESS"] = address

    service = subprocess.Popen(
        [sys.executable, __file__, "--serve", address], env=env
    )
    try:
        time.sleep(0.5)
        result = subprocess.run([binary], env=env, check=False)
        return result.returncode
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
        if len(sys.argv) < 3:
            print(
                "usage: run_dbusutils_realbus.py --serve <address>",
                file=sys.stderr,
            )
            return 2
        return serve(sys.argv[2])

    if len(sys.argv) != 2:
        print("usage: run_dbusutils_realbus.py <binary>", file=sys.stderr)
        return 2

    return run_binary(sys.argv[1])


if __name__ == "__main__":
    raise SystemExit(main())
