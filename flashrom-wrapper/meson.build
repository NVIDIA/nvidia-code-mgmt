dbus_utils_dir = '../src'

inc_dirs = include_directories(['.', '../src/'])

source_files = [
    'flashrom_wrapper.cpp',
    join_paths(dbus_utils_dir, 'dbusutils.cpp'),
]

# Generate config header for flashrom-wrapper
config_h = configure_file(output: 'config.h', configuration: cdata)

executable(
    'flashrom-wrapper',
    sources: source_files,
    include_directories: inc_dirs,
    dependencies: [phosphor_logging, sdbusplus, libgpiocxx, boost],
    install: true,
    install_dir: get_option('bindir'),
)
