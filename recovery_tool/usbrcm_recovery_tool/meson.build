configure_file(output: 'config.h', configuration: cdata)

inc_dirs = include_directories('.')

libusb = dependency('libusb-1.0')

usbrcm_recovery_source_files = [
    'main.cpp',
    'force_recovery.cpp',
    'get_recovery_status.cpp',
    'perform_rcm_recovery.cpp',
    'progress_code_queue.cpp',
    'usb_device_manager.cpp',
    'usb_io.cpp',
]

executable(
    'usbrcm-recovery-tool',
    sources: usbrcm_recovery_source_files,
    include_directories: inc_dirs,
    dependencies: [CLI11_dep, nlohmann_json, libusb],
    install: true,
    install_dir: get_option('bindir'),
)
