unit_files = []

if get_option('PSU_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.PSU.Updater.service',
                                'psu-update@.service']
endif

if get_option('FPGA_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.FPGA.Updater.service',
                                'fpga-update@.service']
endif

if get_option('CPLD_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.CPLD.Updater.service',
                                'cpld-update@.service']
endif

if get_option('RT_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.RT.Updater.service',
                                'rt-update@.service',
                                'rt-update-direct@.service']
endif

if get_option('PEX_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.PEX.Updater.service',
                                'pex-update@.service']
endif

if get_option('DEBUG_TOKEN_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.DebugTokenInstall.Updater.service',
                                'com.Nvidia.DebugTokenErase.Updater.service',
                                'debug-token-update@.service']
endif

foreach unit : unit_files
configure_file(input: unit,
               output: unit,
               copy: true,
               install_dir: servicedir)
endforeach
