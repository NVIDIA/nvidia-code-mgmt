inc_dirs = include_directories(['.', '../src/',
    '../recovery_tool/ocp_recovery_tool/',
    '../recovery_tool/glacier_recovery_tool/',
    '../recovery_tool/common',
    ])

recovery_common_dir = '../recovery_tool/common'
ocp_recovery_dir = '../recovery_tool/ocp_recovery_tool'
glacier_recovery_dir = '../recovery_tool/glacier_recovery_tool'
dbus_utils_dir = '../src'

source_files = [
    'fw-status.cpp',
    'mctp_discovery_resource.cpp',
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
    join_paths(ocp_recovery_dir, 'recoverytool_utils.cpp'),
    join_paths(ocp_recovery_dir, 'recovery_commands.cpp'),
    join_paths(glacier_recovery_dir, 'glacier_recovery_commands.cpp'),
    join_paths(recovery_common_dir, 'i2c_utils.cpp'),
]

executable('fw-status',
            sources: source_files,
            include_directories: inc_dirs,
            cpp_args: compiler_args,
            dependencies:[
                   phosphor_logging,
                   phosphor_dbus_interfaces,
                   sdbusplus,
                   fmt,
                   ssl,
               ],
            install: true,
            install_dir: get_option('bindir')
)
