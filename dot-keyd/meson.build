deps = [
    dependency('sdbusplus'),
    dependency('boost'),
    dependency('openssl'),
    dependency('nlohmann_json'),
    dependency('phosphor-logging'),
]

# Bake an optional CAK public key into the binary. The 'DOT_KEYD_CAK_KEY'
# option value (a raw PEM string, possibly with escaped "\n") is substituted
# verbatim into a raw string literal in the generated header, so no escaping is
# required. Empty option => no baked key.
cak_key_conf = configuration_data()
cak_key_opt = get_option('DOT_KEYD_CAK_KEY')
cak_key_conf.set('cak_key_baked', (cak_key_opt != '') ? 'true' : 'false')
cak_key_conf.set('cak_key_pem', cak_key_opt)

cak_baked_key_hdr = configure_file(
    input: 'src/cak_baked_key.hpp.in',
    output: 'cak_baked_key.hpp',
    configuration: cak_key_conf,
)

executable(
    'dot-keyd',
    'src/cak.cpp',
    'src/dot_installer.cpp',
    'src/dbus_installer.cpp',
    'src/hmc_installer.cpp',
    'src/binary_installer.cpp',
    'src/dot-keyd.cpp',
    cak_baked_key_hdr,
    include_directories: include_directories('.'),
    dependencies: deps,
    override_options: ['cpp_std=c++23'],
    install: true,
)
