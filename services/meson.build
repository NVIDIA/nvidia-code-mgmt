unit_files = [
    'com.Nvidia.PSU.Updater.service',
    'psu-update@.service',
    'com.Nvidia.FPGA.Updater.service',
    'fpga-update@.service',
    'com.Nvidia.CPLD.Updater.service',
    'cpld-update@.service',
    'com.Nvidia.RT.Updater.service',
    'rt-update@.service',
    'com.Nvidia.PEX.Updater.service',
    'pex-update@.service',
]

foreach unit : unit_files
configure_file(input: unit,
               output: unit,
               copy: true,
               install_dir: servicedir)
endforeach
