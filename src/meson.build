configure_file(output: 'config.h',
            configuration: cdata, 
)
psu_inc = include_directories('.')

source_files = [
    'main.cpp',
    'version.cpp',
    'version.cpp',
    'watch.cpp',
    'base_controller.cpp',
    'dbusutils.cpp',
    'base_item_updater.cpp'
]

if get_option('PSU_SUPPORT').enabled()
    source_files += 'psu_updater.cpp'
endif

if get_option('FPGA_SUPPORT').enabled()
    source_files += 'fpga_updater.cpp'
endif

if get_option('CPLD_SUPPORT').enabled()
    source_files += 'cpld_updater.cpp'
endif

if get_option('RT_SUPPORT').enabled()
    source_files += 'retimer_updater.cpp'
endif

if get_option('PEX_SUPPORT').enabled()
    source_files += 'pex_updater.cpp'
endif

if get_option('ORIN_FLASH_SUPPORT').enabled()
    source_files += 'orin_updater.cpp'
endif

if get_option('DEBUG_TOKEN_SUPPORT').enabled()
    source_files += 'debug_token_install.cpp'
    source_files += 'debug_token_erase.cpp'
endif

if get_option('MTD_UPDATER_SUPPORT').enabled()
    source_files += 'mtd_updater.cpp'
endif

if get_option('SWITCHTEC_FUSE_SUPPORT').enabled()
    source_files += 'switchtec_fuse.cpp'
endif

if get_option('JAMPLAYER_SUPPORT').enabled()
    source_files += 'jamplayer.cpp'
endif


executable('code-manager',
            sources: source_files,
            include_directories: psu_inc,
            cpp_args: compiler_args,
            dependencies:[
                   phosphor_logging,
                   phosphor_dbus_interfaces,
                   sdbusplus,
                   fmt,
                   ssl,
               ],
            install: true,
            install_dir: get_option('bindir')
)
