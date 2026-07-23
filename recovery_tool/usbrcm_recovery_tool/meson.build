configure_file(output: 'config.h', configuration: cdata)

common_dir = '../common'
dbus_utils_dir = '../../src'

# Base include directories
inc_dir_list = ['.', '../../src/', '../common']

inc_dirs = include_directories(inc_dir_list)

# USB RCM library source files (shared by both executables)
usb_rcm_lib_sources = [
    'force_recovery.cpp',
    'get_recovery_status.cpp',
    'perform_rcm_recovery.cpp',
    'usb_device_manager.cpp',
    'usb_io.cpp',
    'progress_code_queue.cpp',
]

libusb_dep = dependency('libusb-1.0', required: true)

# 1. Standalone CLI tool (perform_rcm_recovery.cpp uses lg2 for diagnostic log)
standalone_source_files = ['main.cpp'] + usb_rcm_lib_sources

executable(
    'usbrcm-recovery-tool',
    sources: standalone_source_files,
    include_directories: inc_dirs,
    dependencies: [libusb_dep, nlohmann_json, CLI11_dep, phosphor_logging,
                   libgpiocxx, sdbusplus],
    install: true,
    install_dir: get_option('bindir'),
)

# 2. D-Bus/Redfish version
recovery_app_source_files = [
    'main_dbus.cpp',
    join_paths(common_dir, 'message_registry.cpp'),
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
] + usb_rcm_lib_sources

executable(
    'usb-rcm-recovery',
    sources: recovery_app_source_files,
    include_directories: inc_dirs,
    cpp_args: compiler_args,
    dependencies: [
        libusb_dep,
        nlohmann_json,
        phosphor_logging,
        sdbusplus,
        phosphor_dbus_interfaces,
        libgpiocxx,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
