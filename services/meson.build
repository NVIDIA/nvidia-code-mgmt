unit_files = []

if get_option('FWSTATUS_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.FWStatus.service']
endif

if get_option('DOT_KEYD_SUPPORT').enabled()
    unit_files = unit_files + ['dot-keyd.service']
endif

if get_option('PREBOOTDIAG_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.nvidia.PreBootDiag.service',
        'com.nvidia.PreBootDiag.PostCodeCallback@.service',
    ]
endif

if get_option('PSU_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.PSU.Updater.service',
        'psu-update@.service',
    ]
endif

if get_option('FPGA_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.FPGA.Updater.service',
        'fpga-update@.service',
    ]
endif

if get_option('CPLD_SUPPORT').enabled()

    unit_files = unit_files + ['cpld-update@.service']
    cpld_model = get_option('CPLD_SUPPORTED_MODEL')
    if cpld_model != ''
        unit_files = unit_files + ['com.Nvidia.CPLD.Updater.service']
    else
        unit_files = unit_files + [
            'com.Nvidia.CPLD_N.Starter.service',
            'com.Nvidia.CPLD_N.Updater@.service',
        ]
    endif
endif

if get_option('RT_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.RT.Updater.service',
        'rt-update@.service',
        'rt-update-direct@.service',
    ]
endif

if get_option('NVME_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.NVMe.Updater.service',
        'nvme-update@.service',
    ]
endif

if get_option('GPU_OCP_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.OCP.GPU.Recovery.service',
        'ocp-recovery@.service',
    ]
endif

if get_option('PEX_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.PEX.Updater.service',
        'pex-update@.service',
    ]
endif

if get_option('ORIN_FLASH_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.ORIN.Updater.service',
        'orin-update@.service',
    ]
endif

if get_option('VHMC_FLASH_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.vHMC.Updater.service',
        'vhmc-update@.service',
    ]
endif

if get_option('SMCU_FLASH_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.SMCU.Updater.service',
        'smcu-update@.service',
    ]
endif

if get_option('SEP_FLASH_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.SEP.Updater.service',
        'sep-update@.service',
    ]
endif

if get_option('IGX_FUSE_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.IGXFUSE.Updater.service',
        'igx-fuse-update@.service',
    ]
endif

if get_option('FLASHROM_WRAPPER_SUPPORT').enabled()
    unit_files = unit_files + ['com.Nvidia.FlashromWrapper.service']
endif

if get_option('DEBUG_TOKEN_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.DebugTokenInstall.Updater.service',
        'com.Nvidia.DebugTokenErase.Updater.service',
        'debug-token-update@.service',
    ]
endif

if get_option('MTD_UPDATER_SUPPORT').enabled()

    unit_files = unit_files + ['mtd-update@.service']

    fpga_model = get_option('FPGA_SUPPORTED_MODEL')
    if (fpga_model != '')
        unit_files = unit_files + ['com.Nvidia.MTD.Updater.fpga1.service']
        fpga_sed_arg = 's/FPGA_MODEL/'
        fpga_sed_arg += fpga_model
        fpga_sed_arg += '/g'
        run_command(
            'sed',
            '-i',
            fpga_sed_arg,
            'com.Nvidia.MTD.Updater.fpga1.service',
            check: true,
        )
    endif

    bmc_model = get_option('BMC_SUPPORTED_MODEL')

    if (bmc_model != '')
        unit_files = unit_files + ['com.Nvidia.MTD.Updater.bmc.service']
        bmc_sed_arg = 's/BMC_MODEL/'
        bmc_sed_arg += bmc_model
        bmc_sed_arg += '/g'
        run_command(
            'sed',
            '-i',
            bmc_sed_arg,
            'com.Nvidia.MTD.Updater.bmc.service',
            check: true,
        )
    endif

    pciechip_model = get_option('PCIECHIP_SUPPORTED_MODEL')

    if (pciechip_model != '')
        unit_files = unit_files + ['com.Nvidia.MTD.Updater.pciechip.service']
        pciechip_sed_arg = 's/PCIECHIP_MODEL/'
        pciechip_sed_arg += pciechip_model
        pciechip_sed_arg += '/g'
        run_command(
            'sed',
            '-i',
            pciechip_sed_arg,
            'com.Nvidia.MTD.Updater.pciechip.service',
            check: true,
        )
    endif

    assert(
        bmc_model != '' or fpga_model != '' or pciechip_model != '',
        'BMC, FPGA, or PCIECHIP model must be set when using MTD updater',
    )
endif

if get_option('SWITCHTEC_FUSE_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.Switchtec.Fuse.service',
        'switchtec-fuser@.service',
    ]
endif

if get_option('JAMPLAYER_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.Jamplayer.service',
        'jamplayer-flash@.service',
    ]
endif

if get_option('VMEPLAYER_SUPPORT').enabled()
    unit_files = unit_files + ['vmeplayer-flash@.service']

    cpld0_model = get_option('VMEPLAYER0_SUPPORTED_MODEL')
    if (cpld0_model != '')
        unit_files = unit_files + ['com.Nvidia.Vmeplayer0.service']
        cpld0_sed_arg = 's/CPLD0_MODEL/'
        cpld0_sed_arg += cpld0_model
        cpld0_sed_arg += '/g'
        run_command(
            'sed',
            '-i',
            cpld0_sed_arg,
            'com.Nvidia.Vmeplayer0.service',
            check: true,
        )
    endif

    cpld1_model = get_option('VMEPLAYER1_SUPPORTED_MODEL')

    if (cpld1_model != '')
        unit_files = unit_files + ['com.Nvidia.Vmeplayer1.service']
        cpld1_sed_arg = 's/CPLD1_MODEL/'
        cpld1_sed_arg += cpld1_model
        cpld1_sed_arg += '/g'
        run_command(
            'sed',
            '-i',
            cpld1_sed_arg,
            'com.Nvidia.Vmeplayer1.service',
            check: true,
        )
    endif

    assert(
        cpld0_model != '' or cpld1_model != '',
        'Lattice CPLD0 or CPLD1 model must be set when using VME updater',
    )
endif

if get_option('GLACIER_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.GlacierRecovery.Updater.service',
        'glacier-recovery@.service',
    ]
endif

if get_option('MCU_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.MCURecovery.Starter.service',
        'com.Nvidia.MCURecovery.Updater@.service',
        'mcu-recovery@.service',
    ]
endif

if get_option('CONNECTX_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.ConnectXRecovery.Updater.service',
        'connectx-recovery@.service',
    ]
endif

if get_option('NVSWITCH_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.NVSwitchRecovery.Updater.service',
        'nvswitch-recovery@.service',
    ]
endif

if get_option('NVLINKMANAGEMENTNIC_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.NVLinkManagementNICRecovery.Updater.service',
        'nvlinkmgmt-nic-recovery@.service',
    ]
endif

if get_option('SPI_PROGRAMMER_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.SpiProgrammer.Updater.service',
        'spi-programmer@.service',
    ]
endif

if get_option('USBRCM_RECOVERY_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.USBRCMRecovery.Updater.service',
        'usb-rcm-recovery@.service',
    ]
endif

if get_option('NVSWITCH_CONFIG_SUPPORT').enabled()
    unit_files = unit_files + [
        'com.Nvidia.Software.NVSwitchConfig.Updater.service',
    ]
endif

foreach unit : unit_files
    configure_file(
        input: unit,
        output: unit,
        copy: true,
        install_dir: servicedir,
    )
endforeach
