configure_file(output: 'config.h',
            configuration: cdata,
)
gtest = dependency('gtest', main: true, disabler: true, required: false)
gmock = dependency('gmock', disabler: true, required: false)

test_headers = include_directories('.')

source_files = ['../debug_token/update_debug_token.cpp']

update_debug_token_test_src = declare_dependency(
          sources: source_files)

test_deps = [
  meson.get_compiler('cpp').find_library('dl'),
  gtest,
  gmock,
]

test_lib = static_library(
  'mock_syscall.cpp',
  implicit_include_directories: false,
  include_directories: test_headers
)

test_dep = declare_dependency(
  dependencies: test_deps,
  include_directories: test_headers,
  link_with : test_lib
)

tests = []
if get_option('DEBUG_TOKEN_SUPPORT').enabled()
  tests += 'test_debug_token_utility'
  tests += 'test_update_debug_token'
endif

foreach t : tests
  test(t, executable(t.underscorify(), t + '.cpp',
                     implicit_include_directories: false,
                     dependencies: [
                      test_dep,
                      sdbusplus,
                      phosphor_dbus_interfaces,
                      update_debug_token_test_src,
                      fmt]),
                      workdir: meson.current_source_dir())
endforeach