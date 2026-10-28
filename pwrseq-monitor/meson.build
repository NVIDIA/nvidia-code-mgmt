# LPU/FPGA Power-Sequence Monitor (lpu-pwrseq-monitord).
# Built only when PWRSEQ_MONITOR_SUPPORT is enabled in the top-level
# meson_options.txt.

pwrseq_inc = include_directories('include')

# ─── Core library — no openbmc deps, fully unit-testable ───────────────────
# Includes the descriptor loader (nlohmann_json), which is declared at the
# top level (nvidia-code-mgmt/meson.build) — header-only on most distros,
# linked when not.
pwrseq_core_sources = [
    'src/types.cpp',
    'src/element_fsm.cpp',
    'src/module_fsm.cpp',
    'src/tray_fsm.cpp',
    'src/i_device.cpp',
    'src/config/descriptor_loader.cpp',
    'src/state_dump.cpp',
]

pwrseq_core_lib = static_library(
    'pwrseq-core',
    pwrseq_core_sources,
    include_directories: pwrseq_inc,
    dependencies: [nlohmann_json],
    install: false,
)

pwrseq_core_dep = declare_dependency(
    link_with: pwrseq_core_lib,
    include_directories: pwrseq_inc,
    dependencies: [nlohmann_json],
)

# ─── Daemon executable ─────────────────────────────────────────────────────
sdeventplus = dependency('sdeventplus')

pwrseq_daemon_sources = [
    'src/main.cpp',
    'src/orchestrator.cpp',
    'src/dbus_manager.cpp',
    'src/gpio_monitor_client.cpp',
    'src/gpio_monitor_read.cpp',
    'src/reporting/state_mapper.cpp',
    'src/reporting/resource_interfaces.cpp',
    'src/hal/driver_common.cpp',
    'src/hal/lpu_driver.cpp',
    'src/hal/fpga_driver.cpp',
]

executable(
    'lpu-pwrseq-monitord',
    pwrseq_daemon_sources,
    include_directories: pwrseq_inc,
    dependencies: [
        pwrseq_core_dep,
        phosphor_logging,
        phosphor_dbus_interfaces,
        sdbusplus,
        sdeventplus,
        fmt,
        libgpiocxx,
    ],
    install: true,
    install_dir: get_option('bindir'),
)

if get_option('tests').enabled()
    subdir('test')
endif
