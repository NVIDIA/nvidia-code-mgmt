configure_file(output: 'config.h',
              configuration : cdata,
              )
executable('igx-orin-version', 'main.cpp', 'orin_manager.cpp',
                'orin.cpp',
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
install_headers('orin_util.hpp')

