configure_file(output: 'config.h', configuration: cdata)

common_dir = '../common'
dbus_utils_dir = '../../src'

inc_dirs = include_directories(['.', '../../src/', '../common'])

# Core recovery logic shared by both executables
usbdfu_lib_sources = ['perform_dfu_recovery.cpp', 'process_utils.cpp']

# 1. Standalone CLI tool (no D-Bus dependency)
executable(
    'usbdfu-recovery-tool',
    sources: ['main.cpp'] + usbdfu_lib_sources,
    include_directories: inc_dirs,
    dependencies: [nlohmann_json, CLI11_dep, phosphor_logging, libgpiocxx],
    install: true,
    install_dir: get_option('bindir'),
)

# 2. systemd / Redfish worker started by usb-dfu-recovery@.service
executable(
    'usb-dfu-recovery',
    sources: [
        'main_dbus.cpp',
        join_paths(common_dir, 'message_registry.cpp'),
        join_paths(dbus_utils_dir, 'dbusutils.cpp'),
    ] + usbdfu_lib_sources,
    include_directories: inc_dirs,
    cpp_args: compiler_args,
    dependencies: [
        nlohmann_json,
        phosphor_logging,
        libgpiocxx,
        sdbusplus,
        phosphor_dbus_interfaces,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
