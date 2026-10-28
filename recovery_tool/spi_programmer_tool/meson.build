# spi-programmer worker started by spi-programmer@.service
executable(
    'spi-programmer',
    sources: ['main.cpp', 'spi_programmer_worker.cpp'],
    dependencies: [sdbusplus, phosphor_logging],
    install: true,
    install_dir: get_option('bindir'),
)
