systemd = dependency('systemd')
servicedir = systemd.get_variable(pkgconfig : 'systemdsystemunitdir')

cdata_igx_orin = configuration_data()

cdata_igx_orin.set_quoted(
    'BUSNAME', 'com.Nvidia.ORIN')

cdata_igx_orin.set_quoted(
    'BASE_INV_PATH', '/xyz/openbmc_project/inventory/system/board')
cdata_igx_orin.set_quoted(
    'SW_INV_PATH', '/xyz/openbmc_project/software')

sdbusplus = dependency('sdbusplus')
sdeventplus = dependency('sdeventplus')
phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
phosphor_logging = dependency('phosphor-logging')

subdir('services')
subdir('src')
