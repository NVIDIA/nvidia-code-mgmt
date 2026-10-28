# Unit tests for the pwrseq-monitor FSM core + descriptor loader.
# Hardware-free; runs in any CI image with GoogleTest available.

# Match the convention used by nvidia-code-mgmt/test/meson.build:
# disabler: true means that if gtest/gmock are not present in the build
# sysroot (e.g. Yocto cross-compile, where googletest is not pulled into
# the recipe-sysroot), the dependency becomes a disabler and every
# executable() / test() that consumes it is silently dropped from the
# build. Locally, with gtest installed, the tests run as before.
gtest = dependency('gtest', main: true, disabler: true, required: false)
gmock = dependency('gmock', disabler: true, required: false)

test(
    'pwrseq_element_fsm',
    executable(
        'test_element_fsm',
        'test_element_fsm.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)

test(
    'pwrseq_module_fsm',
    executable(
        'test_module_fsm',
        'test_module_fsm.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)

test(
    'pwrseq_tray_fsm',
    executable(
        'test_tray_fsm',
        'test_tray_fsm.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)

test(
    'pwrseq_descriptor_loader',
    executable(
        'test_descriptor_loader',
        'test_descriptor_loader.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)

# SubscriberList is a header-only helper used by SharedLine::dispatch — see
# include/hal/subscriber_list.hpp. The test only needs the include path.
test(
    'pwrseq_subscriber_list',
    executable(
        'test_subscriber_list',
        'test_subscriber_list.cpp',
        include_directories: pwrseq_inc,
        dependencies: [gtest],
    ),
)

test(
    'pwrseq_pg_bc_latch',
    executable(
        'test_pg_bc_latch',
        'test_pg_bc_latch.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)

test(
    'pwrseq_gpio_property_logic',
    executable(
        'test_gpio_property_logic',
        'test_gpio_property_logic.cpp',
        include_directories: pwrseq_inc,
        dependencies: [gtest],
    ),
)

test(
    'pwrseq_state_dump',
    executable(
        'test_state_dump',
        'test_state_dump.cpp',
        dependencies: [pwrseq_core_dep, gtest],
    ),
)
