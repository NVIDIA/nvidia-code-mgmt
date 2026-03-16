configure_file(output: 'config.h', configuration: cdata)
psu_inc = include_directories('.')

source_files = [
    'main.cpp',
    'version.cpp',
    'version.cpp',
    'watch.cpp',
    'base_controller.cpp',
    'dbusutils.cpp',
    'base_item_updater.cpp',
    'signature_verifier.cpp',
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

if get_option('NVME_SUPPORT').enabled()
    source_files += 'nvme_updater.cpp'
endif

if get_option('GPU_OCP_RECOVERY_SUPPORT').enabled()
    source_files += 'ocp_recovery.cpp'
endif

if get_option('GLACIER_RECOVERY_SUPPORT').enabled()
    source_files += 'glacier_crisis_recovery.cpp'
endif

if get_option('PEX_SUPPORT').enabled()
    source_files += 'pex_updater.cpp'
endif

if get_option('ORIN_FLASH_SUPPORT').enabled()
    source_files += 'orin_updater.cpp'
endif

if get_option('VHMC_FLASH_SUPPORT').enabled()
    source_files += 'vhmc_updater.cpp'
endif

if get_option('SMCU_FLASH_SUPPORT').enabled()
    source_files += 'smcu_updater.cpp'
endif

if get_option('SEP_FLASH_SUPPORT').enabled()
    source_files += 'sep_updater.cpp'
endif

if get_option('IGX_FUSE_SUPPORT').enabled()
    source_files += 'igx_fuse_updater.cpp'
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

if get_option('VMEPLAYER_SUPPORT').enabled()
    source_files += 'vmeplayer.cpp'
endif

if get_option('MCU_RECOVERY_SUPPORT').enabled()
    source_files += 'mcu_recovery.cpp'
endif

if get_option('CONNECTX_RECOVERY_SUPPORT').enabled()
    source_files += 'connectx_recovery.cpp'
endif

if get_option('NVSWITCH_RECOVERY_SUPPORT').enabled()
    source_files += 'nvswitch_recovery.cpp'
endif

if get_option('NVLINKMANAGEMENTNIC_RECOVERY_SUPPORT').enabled()
    source_files += 'nvlinkmgmt_nic_recovery.cpp'
endif

if get_option('SPI_PROGRAMMER_SUPPORT').enabled()
    source_files += 'spi_programmer.cpp'
endif

if get_option('USBRCM_RECOVERY_SUPPORT').enabled()
    source_files += 'usb_rcm_recovery.cpp'
endif

executable(
    'code-manager',
    sources: source_files,
    include_directories: psu_inc,
    cpp_args: compiler_args,
    dependencies: [
        phosphor_logging,
        phosphor_dbus_interfaces,
        sdbusplus,
        fmt,
        ssl,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
