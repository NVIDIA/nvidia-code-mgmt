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

if get_option('MTD_UPDATER_SUPPORT').enabled()

    unit_files = unit_files + ['mtd-update@.service']

    fpga_model = get_option('FPGA_SUPPORTED_MODEL')
    if (fpga_model != '')
        unit_files = unit_files + ['com.Nvidia.MTD.Updater.fpga1.service']
        fpga_sed_arg = 's/FPGA_MODEL/'
        fpga_sed_arg += fpga_model
        fpga_sed_arg += '/g'
        run_command('sed', '-i', fpga_sed_arg, 'com.Nvidia.MTD.Updater.fpga1.service', check: true)
    endif

    bmc_model = get_option('BMC_SUPPORTED_MODEL')

    if (bmc_model != '')
        unit_files = unit_files + ['com.Nvidia.MTD.Updater.bmc.service']
        bmc_sed_arg = 's/BMC_MODEL/'
        bmc_sed_arg +=bmc_model
        bmc_sed_arg += '/g'
        run_command('sed', '-i', bmc_sed_arg, 'com.Nvidia.MTD.Updater.bmc.service', check: true)
    endif

    assert(bmc_model != '' or fpga_model !='', 'BMC or FPGA model must be set when using MTD updater')
endif

if get_option('SWITCHTEC_FUSE_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.Switchtec.Fuse.service',
                                'switchtec-fuser@.service']
endif

if get_option('JAMPLAYER_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.Jamplayer.service',
                                'jamplayer-flash@.service']
endif

foreach unit : unit_files
configure_file(input: unit,
               output: unit,
               copy: true,
               install_dir: servicedir)
endforeach
