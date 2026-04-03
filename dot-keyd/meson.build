deps = [
    dependency('sdbusplus'),
    dependency('boost'),
    dependency('openssl'),
    dependency('nlohmann_json'),
    dependency('phosphor-logging'),
]

executable(
    'dot-keyd',
    'src/cak.cpp',
    'src/dot_installer.cpp',
    'src/dbus_installer.cpp',
    'src/hmc_installer.cpp',
    'src/binary_installer.cpp',
    'src/dot-keyd.cpp',
    dependencies: deps,
    override_options: ['cpp_std=c++23'],
    install: true,
)
