prebootdiag_deps = [
    dependency('sdbusplus'),
    dependency('boost'),
    dependency('phosphor-logging'),
    dependency('libgpiodcxx'),
]

prebootdiag_sources = [
    'src/prebootdiag.cpp',
    'src/gpio_handler.cpp',
    'src/dbus_handler.cpp',
]

prebootdiag_lib = static_library(
    'prebootdiag_lib',
    prebootdiag_sources,
    dependencies: prebootdiag_deps,
    include_directories: include_directories('src'),
    override_options: ['cpp_std=c++23'],
)

prebootdiag_dep = declare_dependency(
    link_with: prebootdiag_lib,
    include_directories: include_directories('src'),
    dependencies: prebootdiag_deps,
)

executable(
    'prebootdiag',
    'src/main.cpp',
    dependencies: prebootdiag_dep,
    override_options: ['cpp_std=c++23'],
    install: true,
)

install_data(
    'scripts/prebootdiag-postcode-callback.sh',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
)

subdir('test')
