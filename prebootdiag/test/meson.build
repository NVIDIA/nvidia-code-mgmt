gtest = dependency('gtest', main: true, disabler: true, required: false)
gmock = dependency('gmock', disabler: true, required: false)

if gtest.found() and gmock.found()
    test_deps = [gtest, gmock, prebootdiag_dep]

    tests = ['test_prebootdiag', 'test_heartbeat_timeout']

    foreach t : tests
        test(
            t,
            executable(
                t.underscorify(),
                t + '.cpp',
                dependencies: test_deps,
                override_options: ['cpp_std=c++23'],
            ),
        )
    endforeach
endif
