configure_file(output: 'config.h', configuration: cdata)
gtest = dependency('gtest', main: true, disabler: true, required: false)
gmock = dependency('gmock', disabler: true, required: false)

test_headers = include_directories('.')

source_files = ['../debug_token/update_debug_token.cpp']

update_debug_token_test_src = declare_dependency(sources: source_files)

test_deps = [meson.get_compiler('cpp').find_library('dl'), gtest, gmock]

test_lib = static_library(
    'mock_syscall.cpp',
    implicit_include_directories: false,
    include_directories: test_headers,
)

test_dep = declare_dependency(
    dependencies: test_deps,
    include_directories: test_headers,
    link_with: test_lib,
)

tests = []
if get_option('DEBUG_TOKEN_SUPPORT').enabled()
    tests += 'test_debug_token_utility'
    tests += 'test_update_debug_token'
endif

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            dependencies: [
                test_dep,
                sdbusplus,
                phosphor_dbus_interfaces,
                update_debug_token_test_src,
                fmt,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach

test(
    'test_cpu_recovery_convergence',
    executable(
        'test_cpu_recovery_convergence',
        'test_cpu_recovery_convergence.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=udev_new',
            '-Wl,--wrap=udev_unref',
            '-Wl,--wrap=udev_monitor_new_from_netlink',
            '-Wl,--wrap=udev_monitor_unref',
            '-Wl,--wrap=udev_monitor_filter_add_match_subsystem_devtype',
            '-Wl,--wrap=udev_monitor_enable_receiving',
            '-Wl,--wrap=udev_monitor_get_fd',
            '-Wl,--wrap=udev_monitor_receive_device',
            '-Wl,--wrap=udev_device_get_action',
            '-Wl,--wrap=udev_device_get_devpath',
            '-Wl,--wrap=udev_device_unref',
        ],
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            nlohmann_json,
        ],
        include_directories: include_directories(
            'cpu_recovery_fakes',
            '../fw-status',
        ),
    ),
    workdir: meson.current_source_dir(),
)

# Severity contract for the MCU recovery tool: enumeration must stay silent,
# genuine recovery failures must report. See test_mcu_recovery_severity.cpp.
test(
    'test_mcu_recovery_severity',
    executable(
        'test_mcu_recovery_severity',
        'test_mcu_recovery_severity.cpp',
        '../recovery_tool/common/message_registry.cpp',
        '../recovery_tool/mcu_recovery_tool/mcu_recovery_manager.cpp',
        '../recovery_tool/mcu_recovery_tool/utils.cpp',
        '../src/dbusutils.cpp',
        implicit_include_directories: false,
        dependencies: [
            test_dep,
            cpp.find_library('usb-1.0', required: true),
            libgpiocxx,
            nlohmann_json,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
        ],
        include_directories: include_directories(
            '../recovery_tool/common',
            '../recovery_tool/mcu_recovery_tool',
            '../src',
        ),
    ),
    workdir: meson.current_source_dir(),
)
