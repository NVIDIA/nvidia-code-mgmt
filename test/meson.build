configure_file(output: 'config.h', configuration: cdata)
gtest = dependency('gtest', main: true, disabler: true, required: false)
gtest_nomain = dependency('gtest', main: false, disabler: true, required: false)
gmock = dependency('gmock', disabler: true, required: false)

test_headers = include_directories('.')
test_compile_args = [
    '-fno-inline',
    '-fno-inline-functions',
    '-fno-inline-small-functions',
    '-fno-default-inline',
    '-fno-jump-tables',
    '-fno-tree-switch-conversion',
]

source_files = [
    '../debug_token/update_debug_token.cpp',
    '../debug_token/nsm_debug_token.cpp',
    '../debug_token/tlv/tlv_decoder.cpp',
]

debug_token_test_lib = static_library(
    'debug_token_test_lib',
    source_files,
    implicit_include_directories: false,
    include_directories: test_headers,
    cpp_args: test_compile_args,
    dependencies: [sdbusplus, phosphor_dbus_interfaces, phosphor_logging, fmt],
)

update_debug_token_test_dep = declare_dependency(
    dependencies: [sdbusplus, phosphor_dbus_interfaces, phosphor_logging, fmt],
    link_with: debug_token_test_lib,
)

test_deps = [meson.get_compiler('cpp').find_library('dl'), gtest, gmock]

test_lib = static_library(
    'mock_syscall.cpp',
    implicit_include_directories: false,
    include_directories: test_headers,
)

test_dep = declare_dependency(
    compile_args: test_compile_args,
    dependencies: test_deps,
    include_directories: test_headers,
    link_with: test_lib,
)

probe_test_dep = declare_dependency(
    compile_args: test_compile_args,
    dependencies: [
        meson.get_compiler('cpp').find_library('dl'),
        gtest_nomain,
        gmock,
    ],
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
            dependencies: [test_dep, update_debug_token_test_dep],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach

test(
    'test_tlv_decoder',
    executable(
        'test_tlv_decoder',
        'test_tlv_decoder.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# Expanded update debug token tests
if get_option('DEBUG_TOKEN_SUPPORT').enabled()
    debug_token_binary_preload = shared_library(
        'debug_token_binary_preload',
        'debug_token_binary_preload.cpp',
        implicit_include_directories: false,
        include_directories: test_headers,
        cpp_args: test_compile_args,
        dependencies: [
            meson.get_compiler('cpp').find_library('dl'),
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
        ],
    )

    test(
        'test_update_debug_token_expanded',
        executable(
            'test_update_debug_token_expanded',
            'test_update_debug_token_expanded.cpp',
            implicit_include_directories: false,
            dependencies: [test_dep, update_debug_token_test_dep],
        ),
        workdir: meson.current_source_dir(),
    )

    test(
        'test_update_debug_token_included',
        executable(
            'test_update_debug_token_included',
            'test_update_debug_token_included.cpp',
            implicit_include_directories: false,
            dependencies: [
                test_dep,
                sdbusplus,
                phosphor_dbus_interfaces,
                phosphor_logging,
                fmt,
            ],
        ),
        workdir: meson.current_source_dir(),
    )

    test(
        'test_debug_token_binary_smoke',
        executable(
            'test_debug_token_binary_smoke',
            'test_debug_token_binary_smoke.cpp',
            implicit_include_directories: false,
            dependencies: [test_dep],
        ),
        workdir: meson.current_source_dir(),
        env: [
            'DEBUG_TOKEN_BINARY=' + update_debug_token_exe.full_path(),
            'DEBUG_TOKEN_PRELOAD_LIBRARY=' + debug_token_binary_preload.full_path(),
        ],
        depends: [update_debug_token_exe, debug_token_binary_preload],
    )

endif

test(
    'test_code_manager_binary_smoke',
    executable(
        'test_code_manager_binary_smoke',
        'test_code_manager_binary_smoke.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep],
    ),
    workdir: meson.current_source_dir(),
    env: ['CODE_MANAGER_BINARY=' + code_manager_exe.full_path()],
    depends: [code_manager_exe],
)

# fw-status base_resource tests (with SdBusMock)
fw_status_inc = include_directories('../fw-status', '../fw-status/mctp_util')
test(
    'test_base_resource',
    executable(
        'test_base_resource',
        'test_base_resource.cpp',
        implicit_include_directories: false,
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
        ],
        include_directories: fw_status_inc,
    ),
    workdir: meson.current_source_dir(),
)

# Debug token main.cpp tests (--wrap=sd_bus_open_system, popen, pclose)
test(
    'test_debug_token_main',
    executable(
        'test_debug_token_main',
        'test_debug_token_main.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=sd_bus_open_system',
            '-Wl,--wrap=popen',
            '-Wl,--wrap=pclose',
        ],
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
        ],
    ),
    workdir: meson.current_source_dir(),
)

# NSM strict mock tests (pldm InSequence pattern)
test(
    'test_nsm_strict_mock',
    executable(
        'test_nsm_strict_mock',
        'test_nsm_strict_mock.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=memfd_create', '-Wl,--wrap=write'],
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# NSM real-bus wrapped tests (real D-Bus + --wrap=sd_bus_call with populated replies)
test(
    'test_nsm_realbus_wrapped',
    executable(
        'test_nsm_realbus_wrapped',
        'test_nsm_realbus_wrapped.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=sd_bus_call',
            '-Wl,--wrap=sd_bus_add_match',
            '-Wl,--wrap=sd_bus_wait',
            '-Wl,--wrap=sd_bus_slot_set_userdata',
            '-Wl,--wrap=sd_bus_get_current_slot',
            '-Wl,--wrap=sd_bus_slot_get_userdata',
            '-Wl,--wrap=memfd_create',
            '-Wl,--wrap=popen',
            '-Wl,--wrap=pclose',
            '-Wl,--wrap=write',
        ],
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

nsm_realbus_alloc_probe = executable(
    'test_nsm_realbus_alloc_probe',
    'test_nsm_realbus_wrapped.cpp',
    implicit_include_directories: false,
    cpp_args: ['-DNSM_ALLOC_PROBE_ONLY'],
    link_args: [
        '-Wl,--wrap=sd_bus_call',
        '-Wl,--wrap=sd_bus_add_match',
        '-Wl,--wrap=sd_bus_wait',
        '-Wl,--wrap=sd_bus_slot_set_userdata',
        '-Wl,--wrap=sd_bus_get_current_slot',
        '-Wl,--wrap=sd_bus_slot_get_userdata',
        '-Wl,--wrap=memfd_create',
        '-Wl,--wrap=popen',
        '-Wl,--wrap=pclose',
        '-Wl,--wrap=write',
    ],
    dependencies: [probe_test_dep, update_debug_token_test_dep],
)

debug_token_alloc_probe = executable(
    'test_debug_token_alloc_probe',
    'test_debug_token_alloc_probe.cpp',
    implicit_include_directories: false,
    link_args: ['-Wl,--wrap=popen', '-Wl,--wrap=pclose'],
    dependencies: [probe_test_dep, update_debug_token_test_dep],
)

test(
    'test_nsm_realbus_alloc_probe_handle_async_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCall',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=192',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_handle_async_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCall',
        'NSM_ALLOC_FAIL_FIRST=193',
        'NSM_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_get_status_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=getTokenStatus',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=192',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_get_status_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=getTokenStatus',
        'NSM_ALLOC_FAIL_FIRST=193',
        'NSM_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_install_v2_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCallInstallV2',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=192',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_install_v2_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCallInstallV2',
        'NSM_ALLOC_FAIL_FIRST=193',
        'NSM_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_erase_v2_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCallEraseV2',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=192',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_erase_v2_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=handleAsyncCallEraseV2',
        'NSM_ALLOC_FAIL_FIRST=193',
        'NSM_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_erase_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenErase',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_erase_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenErase',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_install_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenInstall',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_install_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenInstall',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_erase_v2_top_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenEraseV2',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_erase_v2_top_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenEraseV2',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_install_v2_top_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenInstallV2',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_nsm_install_v2_top_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=nsmTokenInstallV2',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_discover_mctp_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=discoverMCTPDevices',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_discover_mctp_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=discoverMCTPDevices',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_update_endpoints_a',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=updateEndPoints',
        'NSM_ALLOC_FAIL_FIRST=1',
        'NSM_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_nsm_realbus_alloc_probe_update_endpoints_b',
    nsm_realbus_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'NSM_ALLOC_PROBE_TARGET=updateEndPoints',
        'NSM_ALLOC_FAIL_FIRST=257',
        'NSM_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_debug_token_alloc_probe_tlv_a',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=tlv',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=1',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=256',
    ],
)

test(
    'test_debug_token_alloc_probe_tlv_b',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=tlv',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=257',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=512',
    ],
)

test(
    'test_debug_token_alloc_probe_token_files',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=token_files',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=1',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_debug_token_alloc_probe_token_parse_a',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=token_parse',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=1',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_debug_token_alloc_probe_token_parse_b',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=token_parse',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=385',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=768',
    ],
)

test(
    'test_debug_token_alloc_probe_update_cmds_a',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    timeout: 120,
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=update_cmds',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=1',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=384',
    ],
)

test(
    'test_debug_token_alloc_probe_update_cmds_b',
    debug_token_alloc_probe,
    workdir: meson.current_source_dir(),
    timeout: 120,
    env: [
        'DEBUG_TOKEN_ALLOC_PROBE_TARGET=update_cmds',
        'DEBUG_TOKEN_ALLOC_FAIL_FIRST=385',
        'DEBUG_TOKEN_ALLOC_FAIL_LAST=768',
    ],
)

# NSM full coverage tests (queue-based D-Bus response mocking)
test(
    'test_nsm_full_coverage',
    executable(
        'test_nsm_full_coverage',
        'test_nsm_full_coverage.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# NSM D-Bus InSequence mocked tests (pldm-style EXPECT_CALL chains)
test(
    'test_nsm_dbus_mocked',
    executable(
        'test_nsm_dbus_mocked',
        'test_nsm_dbus_mocked.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# NSM D-Bus wrapped tests (--wrap=sd_bus_call, popen, sd_bus_open_system)
test(
    'test_nsm_dbus_wrapped',
    executable(
        'test_nsm_dbus_wrapped',
        'test_nsm_dbus_wrapped.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=sd_bus_call',
            '-Wl,--wrap=popen',
            '-Wl,--wrap=pclose',
            '-Wl,--wrap=sd_bus_open_system',
        ],
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    env: [
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'lsan.supp',
    ],
    workdir: meson.current_source_dir(),
)

# NSM debug token deep coverage tests
test(
    'test_nsm_debug_token_deep',
    executable(
        'test_nsm_debug_token_deep',
        'test_nsm_debug_token_deep.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# NSM method-wrap tests (--wrap on UpdateDebugToken helper methods)
# Debug token popen-wrapped tests (--wrap=popen, --wrap=pclose)
test(
    'test_debug_token_popen_wrap',
    executable(
        'test_debug_token_popen_wrap',
        'test_debug_token_popen_wrap.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=popen', '-Wl,--wrap=pclose'],
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# Debug token private method tests (using #define private public)
test(
    'test_debug_token_private',
    executable(
        'test_debug_token_private',
        'test_debug_token_private.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, update_debug_token_test_dep],
    ),
    workdir: meson.current_source_dir(),
)

# Version tests (with SdBusMock + #define private public)
src_inc = include_directories('../src')
version_test_src = declare_dependency(
    sources: ['../src/dbusutils.cpp', '../src/version.cpp'],
)
test(
    'test_version',
    executable(
        'test_version',
        'test_version.cpp',
        implicit_include_directories: false,
        cpp_args: compiler_args,
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            ssl,
            version_test_src,
        ],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

# CPLD resource tests (with SdBusMock + #define private public)
cpld_test_src = declare_dependency(sources: ['../src/dbusutils.cpp'])
test(
    'test_cpld_resource',
    executable(
        'test_cpld_resource',
        'test_cpld_resource.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=sd_bus_call', '-Wl,--wrap=sd_bus_add_match'],
        cpp_args: compiler_args,
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            ssl,
            cpld_test_src,
        ],
        include_directories: include_directories('../fw-status', '../src'),
    ),
    workdir: meson.current_source_dir(),
)

# ConnectX resource tests (reuse the richer fw_status resource harness)
fw_status_base_resource_src = declare_dependency(
    sources: ['fw_status_fakes/base_resource.cpp'],
)
connectx_test_src = declare_dependency(
    sources: ['../fw-status/mctp_discovery_resource.cpp'],
)
test(
    'test_connectx_resource',
    executable(
        'test_connectx_resource',
        'test_connectx_resource.cpp',
        implicit_include_directories: false,
        cpp_args: ['-Wno-subobject-linkage'],
        link_args: [
            '-Wl,--wrap=open',
            '-Wl,--wrap=open64',
            '-Wl,--wrap=clock_nanosleep',
            '-Wl,--wrap=nanosleep',
            '-Wl,--wrap=sleep',
            '-Wl,--wrap=usleep',
        ],
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            connectx_test_src,
            fw_status_base_resource_src,
        ],
        include_directories: include_directories(
            'fw_status_fakes',
            '../fw-status',
            '../fw-status/mctp_util',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
)

# Recovery tool parser tests (pure utility functions)
recovery_parsers_inc = include_directories(
    '../recovery_tool/usbrcm_recovery_tool',
)
test(
    'test_recovery_parsers',
    executable(
        'test_recovery_parsers',
        'test_recovery_parsers.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep],
        include_directories: recovery_parsers_inc,
    ),
    workdir: meson.current_source_dir(),
)
test(
    'test_usbrcm_lowlevel',
    executable(
        'test_usbrcm_lowlevel',
        'test_usbrcm_lowlevel.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=system',
            '-Wl,--wrap=libusb_control_transfer',
            '-Wl,--wrap=libusb_get_device',
            '-Wl,--wrap=libusb_get_device_descriptor',
            '-Wl,--wrap=libusb_get_string_descriptor_ascii',
            '-Wl,--wrap=libusb_error_name',
            '-Wl,--wrap=libusb_strerror',
        ],
        dependencies: [test_dep, nlohmann_json],
        include_directories: recovery_parsers_inc,
    ),
    workdir: meson.current_source_dir(),
)
test(
    'test_usb_device_manager',
    executable(
        'test_usb_device_manager',
        'test_usb_device_manager.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=libusb_init',
            '-Wl,--wrap=libusb_exit',
            '-Wl,--wrap=libusb_set_option',
            '-Wl,--wrap=libusb_error_name',
            '-Wl,--wrap=libusb_strerror',
            '-Wl,--wrap=libusb_get_device_descriptor',
            '-Wl,--wrap=libusb_get_active_config_descriptor',
            '-Wl,--wrap=libusb_free_config_descriptor',
            '-Wl,--wrap=libusb_get_port_numbers',
            '-Wl,--wrap=libusb_get_bus_number',
            '-Wl,--wrap=libusb_open',
            '-Wl,--wrap=libusb_kernel_driver_active',
            '-Wl,--wrap=libusb_detach_kernel_driver',
            '-Wl,--wrap=libusb_set_configuration',
            '-Wl,--wrap=libusb_claim_interface',
            '-Wl,--wrap=libusb_release_interface',
            '-Wl,--wrap=libusb_attach_kernel_driver',
            '-Wl,--wrap=libusb_close',
            '-Wl,--wrap=libusb_get_device_list',
            '-Wl,--wrap=libusb_free_device_list',
            '-Wl,--wrap=libusb_ref_device',
            '-Wl,--wrap=libusb_unref_device',
        ],
        dependencies: [test_dep],
        include_directories: recovery_parsers_inc,
    ),
    workdir: meson.current_source_dir(),
)
test(
    'test_get_recovery_status',
    executable(
        'test_get_recovery_status',
        'test_get_recovery_status.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, nlohmann_json],
        include_directories: recovery_parsers_inc,
    ),
    workdir: meson.current_source_dir(),
)
test(
    'test_usb_i2c_mapper',
    executable(
        'test_usb_i2c_mapper',
        'test_usb_i2c_mapper.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep],
        include_directories: include_directories(
            '../recovery_tool/ocp_recovery_tool',
        ),
    ),
    workdir: meson.current_source_dir(),
)
ocp_recovery_test_src = declare_dependency(
    sources: [
        '../recovery_tool/ocp_recovery_tool/recovery_commands.cpp',
        '../recovery_tool/ocp_recovery_tool/recoverytool_utils.cpp',
        '../recovery_tool/ocp_recovery_tool/usb_i2c_mapper.cpp',
    ],
)
test(
    'test_ocp_recovery',
    executable(
        'test_ocp_recovery',
        'test_ocp_recovery.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=open', '-Wl,--wrap=open64', '-Wl,--wrap=close'],
        dependencies: [
            test_dep,
            fmt,
            nlohmann_json,
            sdbusplus,
            ocp_recovery_test_src,
        ],
        include_directories: include_directories(
            '../recovery_tool/ocp_recovery_tool',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
    timeout: 120,
)
glacier_recovery_test_src = declare_dependency(
    sources: [
        '../recovery_tool/glacier_recovery_tool/glacier_recovery_commands.cpp',
        '../recovery_tool/glacier_recovery_tool/glacier_recovery_utils.cpp',
    ],
)
test(
    'test_glacier_recovery',
    executable(
        'test_glacier_recovery',
        'test_glacier_recovery.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=open', '-Wl,--wrap=open64', '-Wl,--wrap=close'],
        dependencies: [test_dep, fmt, nlohmann_json, glacier_recovery_test_src],
        include_directories: include_directories(
            '../recovery_tool/glacier_recovery_tool',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
    timeout: 120,
)
test(
    'test_glacier_interface_fake',
    executable(
        'test_glacier_interface_fake',
        'test_glacier_interface_fake.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, nlohmann_json, CLI11_dep],
        include_directories: include_directories(
            '../recovery_tool/glacier_recovery_tool',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
)
glacier_main_test_lib = static_library(
    'glacier_main_test_lib',
    '../recovery_tool/glacier_recovery_tool/main.cpp',
    implicit_include_directories: false,
    cpp_args: [
        '-fno-inline',
        '-DGlacierRecoveryCommands=FakeGlacierRecoveryCommands',
        '-Dmain=glacier_unused_main',
        '-Dsleep=fake_sleep',
    ],
    include_directories: include_directories(
        'glacier_main_fakes',
        '../recovery_tool/glacier_recovery_tool',
        '../recovery_tool/common',
        '../fw-status/mctp_util',
    ),
)
glacier_preload = shared_library(
    'glacier_preload',
    'glacier_preload.cpp',
    implicit_include_directories: false,
    dependencies: [meson.get_compiler('cpp').find_library('dl')],
    install: false,
    override_options: ['b_lto=false'],
)
test(
    'test_glacier_main',
    executable(
        'test_glacier_main',
        'test_glacier_main.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep],
        link_with: glacier_main_test_lib,
        include_directories: include_directories(
            'glacier_main_fakes',
            '../recovery_tool/glacier_recovery_tool',
            '../recovery_tool/common',
            '../fw-status/mctp_util',
        ),
    ),
    workdir: meson.current_source_dir(),
    timeout: 120,
)
python3 = find_program('python3')
test(
    'test_glacier_recovery_binary',
    python3,
    args: [
        meson.current_source_dir() / 'run_glacier_recovery_integration.py',
        glacier_recovery_exe.full_path(),
        glacier_preload.full_path(),
    ],
    env: [
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'lsan.supp',
    ],
    workdir: meson.current_source_dir(),
    depends: [glacier_recovery_exe, glacier_preload],
)

# Signature verifier tests (OpenSSL crypto functions)
src_inc = include_directories('../src')
test(
    'test_signature_verifier',
    executable(
        'test_signature_verifier',
        'test_signature_verifier.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, phosphor_logging, ssl],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

base_updater_controller_src = declare_dependency(
    sources: [
        '../src/base_controller.cpp',
        '../src/base_item_updater.cpp',
        '../src/dbusutils.cpp',
        '../src/debug_token_erase.cpp',
        '../src/debug_token_install.cpp',
        '../src/signature_verifier.cpp',
        '../src/version.cpp',
        '../src/watch.cpp',
    ],
)
test(
    'test_base_updater_controller',
    executable(
        'test_base_updater_controller',
        'test_base_updater_controller.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=sd_event_loop'],
        cpp_args: compiler_args,
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            ssl,
            base_updater_controller_src,
        ],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

test(
    'test_code_manager_main',
    executable(
        'test_code_manager_main',
        'test_code_manager_main.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=_Znwm'],
        cpp_args: compiler_args + ['-DDEBUG_TOKEN_SUPPORT'],
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            ssl,
            base_updater_controller_src,
        ],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

watch_test_src = declare_dependency(sources: ['../src/watch.cpp'])
test(
    'test_watch',
    executable(
        'test_watch',
        'test_watch.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=inotify_init1',
            '-Wl,--wrap=inotify_add_watch',
            '-Wl,--wrap=read',
            '-Wl,--wrap=__read_chk',
            '-Wl,--wrap=sd_event_add_io',
        ],
        dependencies: [
            test_dep,
            systemd,
            sdbusplus,
            phosphor_logging,
            watch_test_src,
        ],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

# Message registry tests (with SdBusMock)
recovery_common_inc = include_directories('../recovery_tool/common')
recovery_i2c_test_src = declare_dependency(
    sources: ['../recovery_tool/common/i2c_utils.cpp'],
)
test(
    'test_i2c_utils',
    executable(
        'test_i2c_utils',
        'test_i2c_utils.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=ioctl'],
        dependencies: [test_dep, recovery_i2c_test_src],
        include_directories: recovery_common_inc,
    ),
    workdir: meson.current_source_dir(),
)

mcu_recovery_fake_inc = include_directories('mcu_fakes')
mcu_recovery_test_src = declare_dependency(
    sources: [
        '../recovery_tool/mcu_recovery_tool/mcu_recovery_manager.cpp',
        '../recovery_tool/mcu_recovery_tool/utils.cpp',
    ],
)
test(
    'test_mcu_recovery',
    executable(
        'test_mcu_recovery',
        'test_mcu_recovery.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=open',
            '-Wl,--wrap=open64',
            '-Wl,--wrap=close',
            '-Wl,--wrap=ioctl',
            '-Wl,--wrap=popen',
            '-Wl,--wrap=pclose',
            '-Wl,--wrap=sleep',
            '-Wl,--wrap=usleep',
            '-Wl,--wrap=libusb_init',
            '-Wl,--wrap=libusb_exit',
            '-Wl,--wrap=libusb_get_port_numbers',
            '-Wl,--wrap=libusb_get_bus_number',
            '-Wl,--wrap=libusb_get_device_address',
            '-Wl,--wrap=libusb_get_device_list',
            '-Wl,--wrap=libusb_free_device_list',
            '-Wl,--wrap=libusb_get_device_descriptor',
            '-Wl,--wrap=libusb_get_active_config_descriptor',
            '-Wl,--wrap=libusb_free_config_descriptor',
            '-Wl,--wrap=libusb_error_name',
        ],
        dependencies: [
            test_dep,
            phosphor_logging,
            nlohmann_json,
            mcu_recovery_test_src,
        ],
        include_directories: include_directories(
            'mcu_fakes',
            '../recovery_tool/mcu_recovery_tool',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
)

fw_status_fake_inc = include_directories('fw_status_fakes')
test(
    'test_fw_status_misc',
    executable(
        'test_fw_status_misc',
        'test_fw_status_misc.cpp',
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
            fw_status_base_resource_src,
        ],
        include_directories: include_directories(
            'fw_status_fakes',
            '../fw-status',
            '../fw-status/force_recovery',
            '../recovery_tool/usbrcm_recovery_tool',
        ),
    ),
    workdir: meson.current_source_dir(),
)

fw_status_resource_test_src = declare_dependency(
    sources: ['../fw-status/mctp_discovery_resource.cpp'],
)
test(
    'test_fw_status_resources',
    executable(
        'test_fw_status_resources',
        'test_fw_status_resources.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=open',
            '-Wl,--wrap=open64',
            '-Wl,--wrap=clock_nanosleep',
            '-Wl,--wrap=nanosleep',
            '-Wl,--wrap=sleep',
            '-Wl,--wrap=usleep',
        ],
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            fw_status_resource_test_src,
            fw_status_base_resource_src,
        ],
        include_directories: include_directories(
            'fw_status_fakes',
            '../fw-status',
            '../fw-status/mctp_util',
            '../recovery_tool/common',
        ),
    ),
    workdir: meson.current_source_dir(),
)

test(
    'test_fw_status_glacier_gpio',
    executable(
        'test_fw_status_glacier_gpio',
        'test_fw_status_glacier_gpio.cpp',
        implicit_include_directories: false,
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            fw_status_base_resource_src,
        ],
        include_directories: include_directories(
            'fw_status_fakes',
            '../fw-status',
            '../fw-status/mctp_util',
        ),
    ),
    workdir: meson.current_source_dir(),
)

fw_status_main_test_src = declare_dependency(
    sources: [
        '../fw-status/mctp_discovery_resource.cpp',
        '../fw-status/ap_resource.cpp',
        '../fw-status/erot_resource.cpp',
        '../fw-status/gpio_resource.cpp',
        '../fw-status/cpld_resource.cpp',
        '../fw-status/usb_rcm_resource.cpp',
        '../fw-status/udev_monitor.cpp',
    ],
)
test(
    'test_fw_status_main',
    executable(
        'test_fw_status_main',
        'test_fw_status_main.cpp',
        implicit_include_directories: false,
        cpp_args: [
            '-U_FORTIFY_SOURCE',
            '-D_FORTIFY_SOURCE=0',
            '-fno-stack-protector',
        ],
        link_args: [
            '-Wl,--wrap=sd_bus_call',
            '-Wl,--wrap=sd_bus_add_match',
            '-Wl,--wrap=sd_event_loop',
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
            fw_status_main_test_src,
            fw_status_base_resource_src,
        ],
        include_directories: include_directories(
            'fw_status_fakes',
            '../fw-status',
            '../fw-status/force_recovery',
            '../fw-status/mctp_util',
            '../recovery_tool/common',
            '../recovery_tool/mcu_recovery_tool',
            '../recovery_tool/ocp_recovery_tool',
            '../recovery_tool/usbrcm_recovery_tool',
        ),
    ),
    workdir: meson.current_source_dir(),
)

test(
    'test_fw_status_mctp_runtime',
    executable(
        'test_fw_status_mctp_runtime',
        'test_fw_status_mctp_runtime.cpp',
        implicit_include_directories: false,
        link_args: [
            '-Wl,--wrap=sendto',
            '-Wl,--wrap=socket',
            '-Wl,--wrap=getsockopt',
            '-Wl,--wrap=bind',
            '-Wl,--wrap=recv',
            '-Wl,--wrap=recvfrom',
            '-Wl,--wrap=sd_event_source_set_enabled',
        ],
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_logging,
            declare_dependency(sources: ['../fw-status/mctp_util/utils.cpp']),
        ],
        include_directories: include_directories(
            '../fw-status',
            '../fw-status/mctp_util',
        ),
    ),
    workdir: meson.current_source_dir(),
)

test(
    'test_fw_status_mctp_discovery',
    executable(
        'test_fw_status_mctp_discovery',
        'test_fw_status_mctp_discovery.cpp',
        implicit_include_directories: false,
        cpp_args: [
            '-Wno-error=attribute-warning',
            '-U_FORTIFY_SOURCE',
            '-D_FORTIFY_SOURCE=0',
        ],
        link_args: [
            '-Wl,--wrap=sd_bus_call',
            '-Wl,--wrap=sendto',
            '-Wl,--wrap=socket',
            '-Wl,--wrap=getsockopt',
            '-Wl,--wrap=bind',
            '-Wl,--wrap=recv',
            '-Wl,--wrap=recvfrom',
            '-Wl,--wrap=sd_event_source_set_enabled',
        ],
        dependencies: [
            test_dep,
            systemd,
            sdeventplus,
            sdbusplus,
            phosphor_logging,
            declare_dependency(sources: ['../fw-status/mctp_util/utils.cpp']),
        ],
        include_directories: include_directories(
            '../fw-status',
            '../fw-status/mctp_util',
        ),
    ),
    env: [
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'lsan.supp',
    ],
    workdir: meson.current_source_dir(),
)

test(
    'test_message_registry',
    executable(
        'test_message_registry',
        'test_message_registry.cpp',
        implicit_include_directories: false,
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
        ],
        include_directories: recovery_common_inc,
    ),
    workdir: meson.current_source_dir(),
)

# MCTP utils tests (no D-Bus dependency)
mctp_utils_inc = include_directories('../fw-status/mctp_util')
test(
    'test_mctp_utils',
    executable(
        'test_mctp_utils',
        'test_mctp_utils.cpp',
        implicit_include_directories: false,
        dependencies: [test_dep, sdbusplus, phosphor_logging],
        include_directories: mctp_utils_inc,
    ),
    workdir: meson.current_source_dir(),
)

# DBUSUtils tests (with SdBusMock)
src_inc = include_directories('../src')
dbusutils_test_src = declare_dependency(sources: ['../src/dbusutils.cpp'])
test(
    'test_dbusutils',
    executable(
        'test_dbusutils',
        'test_dbusutils.cpp',
        implicit_include_directories: false,
        link_args: ['-Wl,--wrap=usleep'],
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            ssl,
            dbusutils_test_src,
        ],
        include_directories: src_inc,
    ),
    workdir: meson.current_source_dir(),
)

dbusutils_realbus_exe = executable(
    'test_dbusutils_realbus',
    'test_dbusutils_realbus.cpp',
    implicit_include_directories: false,
    dependencies: [
        test_dep,
        sdbusplus,
        phosphor_dbus_interfaces,
        phosphor_logging,
        fmt,
        ssl,
        base_updater_controller_src,
    ],
    include_directories: src_inc,
)

test(
    'test_dbusutils_realbus',
    python3,
    args: [
        meson.current_source_dir() / 'run_dbusutils_realbus.py',
        dbusutils_realbus_exe.full_path(),
    ],
    workdir: meson.current_source_dir(),
    depends: [dbusutils_realbus_exe],
)

# fw-status MCTP discovery resource tests (with SdBusMock)
mctp_test_src = declare_dependency(
    sources: ['../fw-status/mctp_discovery_resource.cpp'],
)
fw_status_full_inc = include_directories(
    'fw_status_fakes',
    '../fw-status',
    '../fw-status/mctp_util',
)
test(
    'test_mctp_discovery_resource',
    executable(
        'test_mctp_discovery_resource',
        'test_mctp_discovery_resource.cpp',
        implicit_include_directories: false,
        dependencies: [
            test_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            fmt,
            mctp_test_src,
        ],
        include_directories: fw_status_full_inc,
    ),
    workdir: meson.current_source_dir(),
)

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
