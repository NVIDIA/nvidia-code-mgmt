configure_file(output: 'config.h', configuration: cdata)
common_dir = '../common'
dbus_utils_dir = '../../src'
inc_dirs = include_directories(['.', '../../src/', '../common'])

source_files = [
    'glacier_recovery_interface.cpp',
    'glacier_recovery_utils.cpp',
    'glacier_recovery_commands.cpp',
    join_paths(common_dir, 'i2c_utils.cpp'),
]

recovery_app_source_files = [
    'main.cpp',
    'glacier_recovery_commands.cpp',
    join_paths(common_dir, 'i2c_utils.cpp'),
    join_paths(common_dir, 'message_registry.cpp'),
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
]

executable(
    'glacier-recovery-tool',
    sources: source_files,
    include_directories: inc_dirs,
    dependencies: [CLI11_dep, nlohmann_json, fmt],
    install: true,
    install_dir: get_option('bindir'),
)

executable(
    'glacier-recovery',
    sources: recovery_app_source_files,
    include_directories: inc_dirs,
    cpp_args: compiler_args,
    dependencies: [
        CLI11_dep,
        nlohmann_json,
        fmt,
        phosphor_logging,
        sdbusplus,
        phosphor_dbus_interfaces,
    ],
    install: true,
    install_dir: get_option('bindir'),
)

