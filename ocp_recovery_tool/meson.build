configure_file(output: 'config.h',
            configuration: cdata,
)
source_files = [
  'recoverytool_interface.cpp',
  'recoverytool_utils.cpp',
  'i2c_utils.cpp',
  'recovery_commands.cpp',
]

executable( 'ocp-recovery-tool',
            sources: source_files,
            implicit_include_directories: false,
            dependencies: [
                CLI11_dep,
                nlohmann_json,
            ],
            install: true,
            install_dir: get_option('bindir')
)

