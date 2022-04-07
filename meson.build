project('code-mgmt', 'cpp',
        version: '0.1', meson_version: '>=0.57.0',
        default_options: [
          'warning_level=3',
          'werror=true',
          'cpp_std=c++20',
          'b_lto=true',
        ])

systemd = dependency('systemd')
fmt = dependency('fmt')
servicedir = systemd.get_pkgconfig_variable('systemdsystemunitdir')
subdir('services')

# Common configurations for src and test
cdata = configuration_data()

cdata.set_quoted('VERSION_IFACE', 'xyz.openbmc_project.Software.Version')
compiler_args = []
if get_option('MOCK_UTILS')
    compiler_args = ['-DMOCK_UTILS', '-O3']
endif
cdata.set_quoted('IMG_UPLOAD_DIR_BASE', '/tmp/pldm/')

if get_option('PSU_SUPPORT').enabled()
  add_project_arguments('-DPSU_SUPPORT', language : ['c','cpp'])
  cdata.set_quoted('PSU_SUPPORTED_MODEL', get_option('PSU_SUPPORTED_MODEL'))
  cdata.set_quoted('PSU_INVENTORY_IFACE', 'xyz.openbmc_project.Inventory.Item.PowerSupply')
  cdata.set_quoted('PSU_BUSNAME_UPDATER', 'com.Nvidia.Software.PSU.Updater')
  cdata.set_quoted('PSU_BUSNAME_INVENTORY', 'com.Nvidia.Powersupply')
  cdata.set_quoted('PSU_UPDATE_SERVICE', 'psu-update@.service')
endif

if get_option('FPGA_SUPPORT').enabled()
  add_project_arguments('-DFPGA_SUPPORT', language : ['c','cpp'])
  cdata.set_quoted('FPGA_SUPPORTED_MODEL', get_option('FPGA_SUPPORTED_MODEL'))
  cdata.set_quoted('FPGA_INVENTORY_IFACE', 'xyz.openbmc_project.Inventory.Item.Chassis')
  cdata.set_quoted('FPGA_BUSNAME_UPDATER', 'com.Nvidia.Software.FPGA.Updater')
  cdata.set_quoted('FPGA_BUSNAME_INVENTORY', 'com.Nvidia.Cec')
  cdata.set_quoted('FPGA_UPDATE_SERVICE', 'fpga-update@.service')
endif

if get_option('CPLD_SUPPORT').enabled()
  add_project_arguments('-DCPLD_SUPPORT', language : ['c','cpp'])
  cdata.set_quoted('CPLD_SUPPORTED_MODEL', get_option('CPLD_SUPPORTED_MODEL'))
  cdata.set_quoted('CPLD_INVENTORY_IFACE', 'xyz.openbmc_project.Inventory.Item.Chassis')
  cdata.set_quoted('CPLD_BUSNAME_UPDATER', 'com.Nvidia.Software.CPLD.Updater')
  cdata.set_quoted('CPLD_BUSNAME_INVENTORY', 'com.Nvidia.Cpld')
  cdata.set_quoted('CPLD_UPDATE_SERVICE', 'cpld-update@.service')
endif

if get_option('RT_SUPPORT').enabled()
  add_project_arguments('-DRT_SUPPORT', language : ['c','cpp'])
  cdata.set_quoted('RT_SUPPORTED_MODEL', get_option('RT_SUPPORTED_MODEL'))
  cdata.set_quoted('RT_INVENTORY_IFACE', 'xyz.openbmc_project.Inventory.Item.Chassis')
  cdata.set_quoted('RT_BUSNAME_UPDATER', 'com.Nvidia.Software.RT.Updater')
  cdata.set_quoted('RT_BUSNAME_INVENTORY', 'com.Nvidia.Rt')
  cdata.set_quoted('RT_UPDATE_SERVICE', 'rt-update@.service')
  cdata.set_quoted('RT_UPDATE_SINGLE_SERVICE', 'rt-update-direct@.service')
endif

if get_option('PEX_SUPPORT').enabled()
  add_project_arguments('-DPEX_SUPPORT', language : ['c','cpp'])
  cdata.set_quoted('PEX_SUPPORTED_MODEL', get_option('PEX_SUPPORTED_MODEL'))
  cdata.set_quoted('PEX_INVENTORY_IFACE', 'xyz.openbmc_project.Inventory.Item.Chassis')
  cdata.set_quoted('PEX_BUSNAME_UPDATER', 'com.Nvidia.Software.PEX.Updater')
  cdata.set_quoted('PEX_BUSNAME_INVENTORY', 'com.Nvidia.Pex')
  cdata.set_quoted('PEX_UPDATE_SERVICE', 'pex-update@.service')
endif

cdata.set_quoted('SOFTWARE_OBJPATH', '/xyz/openbmc_project/software/other')
cdata.set_quoted('ACTIVATE_INTERFACE', 'xyz.openbmc_project.Software.Activation')


cdata.set_quoted('INVENTORY_PATH', '/xyz/openbmc_project/inventory/')

cdata.set_quoted('MAPPER_BUSNAME', 'xyz.openbmc_project.ObjectMapper')
cdata.set_quoted('MAPPER_INTERFACE', 'xyz.openbmc_project.ObjectMapper')
cdata.set_quoted('MAPPER_PATH', '/xyz/openbmc_project/object_mapper')

cdata.set_quoted('ITEM_IFACE', 'xyz.openbmc_project.Inventory.Item')
cdata.set_quoted('ASSET_IFACE', 'xyz.openbmc_project.Inventory.Decorator.Asset')
cdata.set_quoted('VERSION_IFACE', 'xyz.openbmc_project.Software.Version')
cdata.set_quoted('FILEPATH_IFACE', 'xyz.openbmc_project.Common.FilePath')
cdata.set_quoted('ACTIVATION_FWD_ASSOCIATION', 'inventory')
cdata.set_quoted('ACTIVATION_REV_ASSOCIATION', 'activation')
cdata.set_quoted('ACTIVE_FWD_ASSOCIATION', 'active')
cdata.set_quoted('ACTIVE_REV_ASSOCIATION', 'software_version')
cdata.set_quoted('FUNCTIONAL_FWD_ASSOCIATION', 'functional')
cdata.set_quoted('FUNCTIONAL_REV_ASSOCIATION', 'software_version')
cdata.set_quoted('UPDATEABLE_FWD_ASSOCIATION', 'updateable')
cdata.set_quoted('UPDATEABLE_REV_ASSOCIATION', 'software_version')
cdata.set_quoted('VERSION', 'Version')
cdata.set_quoted('PRESENT', 'Present')
cdata.set_quoted('MANUFACTURER', 'Manufacturer')
cdata.set_quoted('MODEL', 'Model')



cdata.set_quoted('SYSTEMD_BUSNAME','org.freedesktop.systemd1')
cdata.set_quoted('SYSTEMD_PATH' ,'/org/freedesktop/systemd1')
cdata.set_quoted('SYSTEMD_INTERFACE' ,'org.freedesktop.systemd1.Manager')

cdata.set_quoted('INVENTORY_PATH_BASE', get_option('INVENTORY_PATH_BASE'))
cdata.set_quoted('IMG_DIR', get_option('IMG_DIR'))
cdata.set_quoted('IMG_DIR_PERSIST', get_option('IMG_DIR_PERSIST'))
cdata.set_quoted('IMG_DIR_BUILTIN', get_option('IMG_DIR_BUILTIN'))

phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
phosphor_logging = dependency('phosphor-logging')
sdbusplus = dependency('sdbusplus')
ssl = dependency('openssl')

add_project_link_arguments(['-lstdc++fs'], language: 'cpp')

subdir('src')

