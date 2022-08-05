configure_file(output: 'config.h',
            configuration: cdata,
)
debug_token_inc = include_directories('.')

source_files = [
    'main.cpp',
    'update_debug_token.cpp'
]

executable('updateDebugToken',
            sources: source_files,
            include_directories: debug_token_inc,
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
