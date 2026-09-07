include_guard(GLOBAL)

function(spark_enable_fuzz_policy source_root)
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

    if(BUILD_TESTS)
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
            TIMEOUT 60)

        add_test(
            NAME FuzzPolicyAdversarial
            COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                -s "${source_root}/Tests/fuzz-policy"
                -p "test_*.py"
                -v)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz-policy-tmp")
        set_tests_properties(FuzzPolicyAdversarial PROPERTIES
            ENVIRONMENT
                "TMP=${CMAKE_BINARY_DIR}/fuzz-policy-tmp;TEMP=${CMAKE_BINARY_DIR}/fuzz-policy-tmp;TMPDIR=${CMAKE_BINARY_DIR}/fuzz-policy-tmp"
            LABELS "security;fuzz-policy;unit"
            WORKING_DIRECTORY "${source_root}"
            TIMEOUT 60)
    endif()
endfunction()
