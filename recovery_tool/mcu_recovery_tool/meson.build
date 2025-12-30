common_dir = '../common'
dbus_utils_dir = '../../src'
inc_dirs = include_directories(['.', '../../src/', '../common', '../../src'])
libusb_dep = cpp.find_library('usb-1.0', required: true)

cli_sources = files(
    join_paths(common_dir, 'message_registry.cpp'),
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
    'mcu_recovery_interfaces.cpp',
    'mcu_recovery_manager.cpp',
    'utils.cpp',
)
executable(
    'mcu-recovery-tool',
    cli_sources,
    cpp_args: compiler_args,
    include_directories: inc_dirs,
    dependencies: [
        libusb_dep,
        nlohmann_json,
        libgpiocxx,
        sdbusplus,
        phosphor_dbus_interfaces,
        phosphor_logging,
    ],
    install: true,
    install_dir: get_option('bindir'),
)

app_sources = files(
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
    join_paths(common_dir, 'message_registry.cpp'),
    'mcu_recovery_app.cpp',
    'mcu_recovery_manager.cpp',
    'utils.cpp',
)
executable(
    'mcu-recovery',
    app_sources,
    cpp_args: compiler_args,
    include_directories: inc_dirs,
    dependencies: [
        libusb_dep,
        nlohmann_json,
        libgpiocxx,
        sdbusplus,
        phosphor_dbus_interfaces,
        phosphor_logging,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
