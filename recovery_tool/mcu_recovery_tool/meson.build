sources = files('mcu_recovery.cpp')
libusb_dep = cpp.find_library('usb-1.0', required: true)
executable('mcu-recovery-tool',
           sources,
           cpp_args: compiler_args,
           dependencies : [
               libusb_dep,
               nlohmann_json,
               libgpiocxx
           ],
           install: true,
           install_dir: get_option('bindir')
)
