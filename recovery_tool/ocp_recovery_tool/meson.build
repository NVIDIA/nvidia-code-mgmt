configure_file(output: 'config.h',
            configuration: cdata,
)
source_files = [
  'recoverytool_interface.cpp',
  'recoverytool_utils.cpp',
  '../common/i2c_utils.cpp',
  'recovery_commands.cpp',
]

common_inc = include_directories('../common')

executable( 'ocp-recovery-tool',
            sources: source_files,
            include_directories: common_inc,
            dependencies: [
                CLI11_dep,
                nlohmann_json,
            ],
            install: true,
            install_dir: get_option('bindir')
)

