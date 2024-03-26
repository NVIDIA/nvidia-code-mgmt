systemd = dependency('systemd')
servicedir = systemd.get_variable(pkgconfig : 'systemdsystemunitdir')

cdata = configuration_data()

cdata.set_quoted(
    'BUSNAME', 'com.Nvidia.ORIN')

cdata.set_quoted(
    'BASE_INV_PATH', '/xyz/openbmc_project/inventory/system/board')
cdata.set_quoted(
    'SW_INV_PATH', '/xyz/openbmc_project/software')

sdbusplus = dependency('sdbusplus')
sdeventplus = dependency('sdeventplus')
phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
phosphor_logging = dependency('phosphor-logging')

subdir('services')
subdir('src')
