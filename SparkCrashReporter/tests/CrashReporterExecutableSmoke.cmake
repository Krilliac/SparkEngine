# Exercise the shipped SparkCrashReporter executable through its CLI boundary.
# This intentionally avoids consent UI and network access so it is deterministic
# on both Windows and POSIX CI runners.

if(NOT DEFINED REPORTER OR REPORTER STREQUAL "")
    message(FATAL_ERROR "REPORTER must point to the built SparkCrashReporter executable")
endif()
if(NOT EXISTS "${REPORTER}")
    message(FATAL_ERROR "SparkCrashReporter executable does not exist: ${REPORTER}")
endif()

if(NOT DEFINED WORK_DIR OR WORK_DIR STREQUAL "")
    message(FATAL_ERROR "WORK_DIR must be an owned smoke-test directory")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
set(MISSING_MANIFEST "${WORK_DIR}/missing-manifest.json")
file(REMOVE "${MISSING_MANIFEST}")

function(run_reporter label expected_status expected_output)
    execute_process(
        COMMAND "${REPORTER}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout_text
        ERROR_VARIABLE stderr_text
        TIMEOUT 10)

    if(result STREQUAL "Process terminated due to timeout")
        message(FATAL_ERROR "${label} timed out after 10 seconds")
    endif()
    if(NOT result EQUAL expected_status)
        message(FATAL_ERROR "${label} returned ${result}, expected ${expected_status}\nstdout:\n${stdout_text}\nstderr:\n${stderr_text}")
    endif()
    string(CONCAT combined_output "${stdout_text}" "${stderr_text}")
    string(FIND "${combined_output}" "${expected_output}" output_offset)
    if(output_offset LESS 0)
        message(FATAL_ERROR "${label} did not emit '${expected_output}'\nstdout:\n${stdout_text}\nstderr:\n${stderr_text}")
    endif()
endfunction()

run_reporter("--version" 0 "SparkCrashReporter" --version)
run_reporter("--help" 0 "Usage:" --help)
run_reporter("missing manifest" 1 "Failed to load manifest" --report "${MISSING_MANIFEST}")
