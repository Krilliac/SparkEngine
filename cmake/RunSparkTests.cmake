cmake_minimum_required(VERSION 3.25)

# SPARK_TEST_TIMEOUT_SECONDS is required, not defaulted: a single hard-coded
# wall clock silently applied to every configuration, and the slow (Debug)
# configuration exceeded it while the suite itself was healthy. Callers must
# state the budget they mean, so a config that needs more can never inherit
# the fast config's number by omission.
foreach(_required SPARK_TEST_EXECUTABLE SPARK_JUNIT_REPORT SPARK_TEST_LOG SPARK_TEST_OUTPUT
                  SPARK_TEST_TIMEOUT_SECONDS)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkTests.cmake requires -D${_required}=<value>")
    endif()
endforeach()

if(NOT "${SPARK_TEST_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "SPARK_TEST_TIMEOUT_SECONDS must be a positive integer")
endif()

set(_junit_path "${SPARK_JUNIT_REPORT}")
set(_log_path "${SPARK_TEST_LOG}")
set(_output_path "${SPARK_TEST_OUTPUT}")
cmake_path(ABSOLUTE_PATH _junit_path NORMALIZE OUTPUT_VARIABLE _junit_path)
cmake_path(ABSOLUTE_PATH _log_path NORMALIZE OUTPUT_VARIABLE _log_path)
cmake_path(ABSOLUTE_PATH _output_path NORMALIZE OUTPUT_VARIABLE _output_path)
if(_junit_path STREQUAL _log_path OR
   _junit_path STREQUAL _output_path OR
   _log_path STREQUAL _output_path)
    message(FATAL_ERROR "SparkTests JUnit, raw log, and runner output paths must be distinct")
endif()

get_filename_component(_junit_dir "${SPARK_JUNIT_REPORT}" DIRECTORY)
get_filename_component(_log_dir "${SPARK_TEST_LOG}" DIRECTORY)
get_filename_component(_output_dir "${SPARK_TEST_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_junit_dir}" "${_log_dir}" "${_output_dir}")
file(REMOVE "${SPARK_JUNIT_REPORT}" "${SPARK_TEST_LOG}" "${SPARK_TEST_OUTPUT}")

if(NOT EXISTS "${SPARK_TEST_EXECUTABLE}")
    message(FATAL_ERROR "SparkTests executable is missing: ${SPARK_TEST_EXECUTABLE}")
endif()

set(_spark_test_command "${SPARK_TEST_EXECUTABLE}")
if(DEFINED SPARK_TEST_DRIVER AND NOT "${SPARK_TEST_DRIVER}" STREQUAL "")
    if(NOT EXISTS "${SPARK_TEST_DRIVER}")
        message(FATAL_ERROR "SparkTests driver is missing: ${SPARK_TEST_DRIVER}")
    endif()
    list(APPEND _spark_test_command "${SPARK_TEST_DRIVER}")
endif()

# The direct-invocation lanes (linux-asan/linux-tsan) already run with
# --warn-is-error; without this switch the ctest lanes had no way to ask for it.
set(_spark_test_flags --quiet)
if(SPARK_TEST_WARN_IS_ERROR)
    list(APPEND _spark_test_flags --warn-is-error)
endif()

# Keep framework output separate from the raw process stream. TestMain needs its
# own file to populate per-test JUnit failures, while CMake must retain stdout and
# raw stderr from log sinks and crash handlers even when TestMain cannot finish.
execute_process(
    COMMAND ${_spark_test_command}
        ${_spark_test_flags}
        --output-file "${SPARK_TEST_OUTPUT}"
        --junit-xml "${SPARK_JUNIT_REPORT}"
    RESULT_VARIABLE _result
    OUTPUT_FILE "${SPARK_TEST_LOG}"
    ERROR_FILE "${SPARK_TEST_LOG}"
    TIMEOUT "${SPARK_TEST_TIMEOUT_SECONDS}"
)

if(NOT "${_result}" STREQUAL "0")
    function(_spark_read_tail _path _output)
        set(_tail "<missing>")
        if(EXISTS "${_path}")
            file(SIZE "${_path}" _size)
            if(_size EQUAL 0)
                set(_tail "<empty>")
            else()
                math(EXPR _offset "${_size} - 32768")
                if(_offset LESS 0)
                    set(_offset 0)
                endif()
                file(READ "${_path}" _tail OFFSET ${_offset} LIMIT 32768)
            endif()
        endif()
        set(${_output} "${_tail}" PARENT_SCOPE)
    endfunction()
    _spark_read_tail("${SPARK_TEST_LOG}" _raw_excerpt)
    _spark_read_tail("${SPARK_TEST_OUTPUT}" _runner_excerpt)
    message(FATAL_ERROR
        "SparkTests failed or could not be launched (result: ${_result}).\n"
        "Raw process output (last up to 32 KiB):\n${_raw_excerpt}\n"
        "Test runner output (last up to 32 KiB):\n${_runner_excerpt}")
endif()

if(NOT EXISTS "${SPARK_JUNIT_REPORT}")
    message(FATAL_ERROR "SparkTests returned success without producing JUnit: ${SPARK_JUNIT_REPORT}")
endif()
file(SIZE "${SPARK_JUNIT_REPORT}" _junit_size)
if(_junit_size EQUAL 0)
    message(FATAL_ERROR "SparkTests returned success with an empty JUnit report: ${SPARK_JUNIT_REPORT}")
endif()

message(STATUS "SparkTests completed successfully; JUnit bytes: ${_junit_size}")
