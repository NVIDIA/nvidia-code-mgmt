configure_file(output: 'config.h',
              configuration : cdata_igx_fuse,
              )
executable('igx-fuse-version', 'main.cpp', 'igx_fuse_manager.cpp',
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
install_headers('igx_fuse_util.hpp')

