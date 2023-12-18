configure_file(output: 'config.h',
            configuration: cdata,
)

common_dir = '../common'
common_inc = include_directories(common_dir)

source_files = [
  'recoverytool_interface.cpp',
  'recoverytool_utils.cpp',
  join_paths(common_dir, 'i2c_utils.cpp'),
  'recovery_commands.cpp',
]

executable( 'ocp-recovery-tool',
            sources: source_files,
            include_directories: common_inc,
            dependencies: [
                fmt,
                CLI11_dep,
                nlohmann_json,
            ],
            install: true,
            install_dir: get_option('bindir')
)

