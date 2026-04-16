boost_dep = dependency('boost', modules: ['system'])

executable(
    'nvswitch-config-manager',
    sources: ['main.cpp', 'nvswitch_config_manager.cpp'],
    include_directories: include_directories('.'),
    dependencies: [phosphor_logging, sdbusplus, fmt, boost_dep],
    install: true,
    install_dir: get_option('bindir'),
)
