include_guard(GLOBAL)

# SEC-120 registers Python-backed policy checks. Python must therefore only be a
# hard requirement when those checks are actually registered: an engine-only
# configure (-DBUILD_TESTS=OFF) must not fail on a machine without Python.
option(SPARK_ENABLE_FUZZ_POLICY_CHECKS
    "Register the blocking SEC-120 fuzz-policy target and CTest checks (requires Python3)"
    ${BUILD_TESTS})

function(spark_enable_fuzz_policy source_root)
    if(NOT SPARK_ENABLE_FUZZ_POLICY_CHECKS)
        message(STATUS "[SEC-120] Fuzz-policy checks disabled (SPARK_ENABLE_FUZZ_POLICY_CHECKS=OFF)")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    if(NOT TARGET check-fuzz-policy)
        add_custom_target(check-fuzz-policy
            COMMAND "${Python3_EXECUTABLE}"
                "${source_root}/tools/fuzz-policy/check_fuzz_policy.py"
                --source-root "${source_root}"
                --ci
            WORKING_DIRECTORY "${source_root}"
            COMMENT "Checking the blocking SEC-120 fuzz policy"
            VERBATIM)
    endif()

    enable_testing()

    add_test(
        NAME FuzzPolicy
        COMMAND "${Python3_EXECUTABLE}"
            "${source_root}/tools/fuzz-policy/check_fuzz_policy.py"
            --source-root "${source_root}"
            --ci)
    set_tests_properties(FuzzPolicy PROPERTIES
        LABELS "security;fuzz-policy"
        WORKING_DIRECTORY "${source_root}"
        TIMEOUT 120)

    # 'unittest discover' does not recurse into directories without __init__.py,
    # so a test file dropped into a subdirectory would never run and the suite
    # would still report success. Refuse to configure in that shape.
    file(GLOB_RECURSE _spark_fuzz_policy_tests LIST_DIRECTORIES false
        "${source_root}/Tests/fuzz-policy/test_*.py")
    list(LENGTH _spark_fuzz_policy_tests _spark_fuzz_policy_test_count)
    if(_spark_fuzz_policy_test_count EQUAL 0)
        message(FATAL_ERROR "[SEC-120] No adversarial policy tests found under Tests/fuzz-policy")
    endif()
    foreach(_spark_fuzz_policy_test IN LISTS _spark_fuzz_policy_tests)
        get_filename_component(_spark_fuzz_policy_test_dir "${_spark_fuzz_policy_test}" DIRECTORY)
        if(NOT _spark_fuzz_policy_test_dir STREQUAL "${source_root}/Tests/fuzz-policy")
            message(FATAL_ERROR
                "[SEC-120] ${_spark_fuzz_policy_test} sits in a subdirectory that "
                "'unittest discover' will not recurse into; move it up or add __init__.py")
        endif()
    endforeach()

    add_test(
        NAME FuzzPolicyAdversarial
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${source_root}/Tests/fuzz-policy"
            -p "test_*.py"
            -v)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz-policy-tmp")
    # A discovery run that finds nothing exits 0 on Python <= 3.11; assert on the
    # output too so "0 tests ran" can never be read as "nothing failed".
    set_tests_properties(FuzzPolicyAdversarial PROPERTIES
        ENVIRONMENT
            "TMP=${CMAKE_BINARY_DIR}/fuzz-policy-tmp;TEMP=${CMAKE_BINARY_DIR}/fuzz-policy-tmp;TMPDIR=${CMAKE_BINARY_DIR}/fuzz-policy-tmp"
        FAIL_REGULAR_EXPRESSION "NO TESTS RAN;Ran 0 tests"
        LABELS "security;fuzz-policy;unit"
        WORKING_DIRECTORY "${source_root}"
        TIMEOUT 300)
endfunction()
