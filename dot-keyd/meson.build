deps = [
    dependency('sdbusplus'),
    dependency('boost'),
    dependency('openssl'),
    dependency('libcurl'),
    dependency('nlohmann_json'),
    dependency('phosphor-logging'),
]

executable(
    'dot-keyd',
    'src/dot-keyd.cpp',
    dependencies: deps,
    override_options: ['cpp_std=c++23'],
    install: true,
)
