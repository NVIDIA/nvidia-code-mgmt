configure_file(output: 'config.h',
              configuration : cdata_igx_smcu,
              )
executable('igx-smcu-version', 'main.cpp', 'smcu_manager.cpp',
               dependencies:
                            [
                              sdbusplus,
                              sdeventplus,
                              phosphor_logging,
                              phosphor_dbus_interfaces,
                              fmt,
                            ],
               install : true,
               install_dir : get_option('bindir'))
install_headers('smcu_util.hpp')

