configure_file(output: 'config.h',
            configuration: cdata,
)
common_dir = '../common'
common_inc = include_directories(common_dir)

source_files = [
  'glacier_recovery_interface.cpp',
  'glacier_recovery_utils.cpp',
  'glacier_recovery_commands.cpp',
  join_paths(common_dir, 'i2c_utils.cpp')
]

executable( 'glacier-recovery-tool',
            sources: source_files,
            include_directories: common_inc,
            dependencies: [
                CLI11_dep,
                nlohmann_json,
                fmt,
            ],
            install: true,
            install_dir: get_option('bindir')
)