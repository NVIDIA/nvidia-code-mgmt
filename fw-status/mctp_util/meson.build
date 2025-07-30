sdeventplus_dep = dependency('sdeventplus')
deps = [
    sdeventplus_dep,
    sdbusplus_dep,
    phosphor_dbus_interfaces_dep,
    phosphor_logging_dep,
]

source = [
    'main.cpp',
    'mctp_endpoint_discovery.cpp',
    'socket_handler.cpp',
    'mctp_vdm_helper.cpp',
    'utils.cpp',
]

executable(
    'erot-time-manager',
    source,
    implicit_include_directories: false,
    dependencies: deps,
    install: true,
    install_dir: get_option('bindir'),
)

configure_file(
    input: 'services/nvidia-erot-time-manager.service',
    output: 'nvidia-erot-time-manager.service',
    copy: true,
    install_dir: systemd_system_unit_dir,
)

