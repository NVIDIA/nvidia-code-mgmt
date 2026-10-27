mctp_util_dir = '../fw-status/mctp_util'
dbus_utils_dir = '../src'

inc_dirs = include_directories(
    [
        '.',
        mctp_util_dir,
        dbus_utils_dir,
    ],
)

source_files = [
    'bios_erot_spi.cpp',
    join_paths(mctp_util_dir, 'socket_handler.cpp'),
    join_paths(mctp_util_dir, 'utils.cpp'),
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
]

sdeventplus = dependency('sdeventplus')

executable(
    'bios-erot-spi',
    sources: source_files,
    include_directories: inc_dirs,
    dependencies: [
        phosphor_logging,
        phosphor_dbus_interfaces,
        sdbusplus,
        sdeventplus,
    ],
    install: true,
    install_dir: get_option('libexecdir') / 'code-mgmt',
)
