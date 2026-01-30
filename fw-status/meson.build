inc_dirs = include_directories(
    [
        '.',
        '../src/',
        '../debug_token/',
        '../recovery_tool/ocp_recovery_tool/',
        '../recovery_tool/glacier_recovery_tool/',
        '../recovery_tool/mcu_recovery_tool/',
        '../recovery_tool/usbrcm_recovery_tool/',
        '../recovery_tool/common',
        './mctp_util',
        './force_recovery',
    ],
)

recovery_common_dir = '../recovery_tool/common'
ocp_recovery_dir = '../recovery_tool/ocp_recovery_tool'
glacier_recovery_dir = '../recovery_tool/glacier_recovery_tool'
mcu_recovery_dir = '../recovery_tool/mcu_recovery_tool'
usbrcm_recovery_dir = '../recovery_tool/usbrcm_recovery_tool'
dbus_utils_dir = '../src'
mctp_util_dir = './mctp_util'

libusb_dep = dependency('libusb-1.0', required: true)
libudev_dep = dependency('libudev', required: true)

source_files = [
    'fw-status.cpp',
    'mctp_discovery_resource.cpp',
    'ap_resource.cpp',
    'erot_resource.cpp',
    'gpio_resource.cpp',
    'usb_rcm_resource.cpp',
    'udev_monitor.cpp',
    join_paths(mctp_util_dir, 'mctp_vdm_helper.cpp'),
    join_paths(mctp_util_dir, 'mctp_endpoint_discovery.cpp'),
    join_paths(mctp_util_dir, 'socket_handler.cpp'),
    join_paths(mctp_util_dir, 'utils.cpp'),
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
    join_paths(ocp_recovery_dir, 'recoverytool_utils.cpp'),
    join_paths(ocp_recovery_dir, 'recovery_commands.cpp'),
    join_paths(ocp_recovery_dir, 'usb_i2c_mapper.cpp'),
    join_paths(glacier_recovery_dir, 'glacier_recovery_commands.cpp'),
    join_paths(recovery_common_dir, 'i2c_utils.cpp'),
    join_paths(recovery_common_dir, 'message_registry.cpp'),
    join_paths(mcu_recovery_dir, 'mcu_recovery_manager.cpp'),
    join_paths(usbrcm_recovery_dir, 'force_recovery.cpp'),
    join_paths(usbrcm_recovery_dir, 'get_recovery_status.cpp'),
    join_paths(usbrcm_recovery_dir, 'usb_device_manager.cpp'),
    join_paths(usbrcm_recovery_dir, 'usb_io.cpp'),
    join_paths(usbrcm_recovery_dir, 'progress_code_queue.cpp'),
    'force_recovery/mcu_recovery_mode_manager.cpp',
    'force_recovery/usbrcm_recovery_manager.cpp',
]

nlohmann_json = dependency('nlohmann_json', required: true)

sdeventplus = dependency('sdeventplus')
executable(
    'fw-status',
    sources: source_files,
    include_directories: inc_dirs,
    cpp_args: compiler_args,
    dependencies: [
        phosphor_logging,
        phosphor_dbus_interfaces,
        sdbusplus,
        sdeventplus,
        fmt,
        ssl,
        libgpiocxx,
        libusb_dep,
        libudev_dep,
        nlohmann_json,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
