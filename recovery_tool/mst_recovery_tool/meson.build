mst_recovery_sources = files(
    '../common/i2c_utils.cpp',
    'mst_platform_real.cpp',
    'mst_policy.cpp',
    'mst_recovery.cpp',
)

mst_recovery_inc = include_directories('.', '../common')

executable(
    'mst-recovery',
    sources: mst_recovery_sources + files(
        'main.cpp',
        'mst_platform_factory.cpp',
    ),
    include_directories: mst_recovery_inc,
    dependencies: [sdbusplus, libgpiocxx, phosphor_logging],
    install: true,
    install_dir: get_option('bindir'),
)
