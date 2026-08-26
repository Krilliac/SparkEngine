cmake_minimum_required(VERSION 3.25)

foreach(_required SPARK_TEST_EXECUTABLE SPARK_JUNIT_REPORT SPARK_TEST_LOG)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkTests.cmake requires -D${_required}=<path>")
    endif()
endforeach()

if(NOT EXISTS "${SPARK_TEST_EXECUTABLE}")
    message(FATAL_ERROR "SparkTests executable is missing: ${SPARK_TEST_EXECUTABLE}")
endif()

get_filename_component(_junit_dir "${SPARK_JUNIT_REPORT}" DIRECTORY)
get_filename_component(_log_dir "${SPARK_TEST_LOG}" DIRECTORY)
file(MAKE_DIRECTORY "${_junit_dir}" "${_log_dir}")
file(REMOVE "${SPARK_JUNIT_REPORT}" "${SPARK_TEST_LOG}")

# Give the child real file handles instead of CTest's captured output pipe.
# SparkTests and engine subsystems can be verbose even in runner quiet mode;
# direct redirection avoids pipe backpressure while retaining full diagnostics.
execute_process(
    COMMAND "${SPARK_TEST_EXECUTABLE}"
        --quiet
        --junit-xml "${SPARK_JUNIT_REPORT}"
    RESULT_VARIABLE _result
    OUTPUT_FILE "${SPARK_TEST_LOG}"
    ERROR_FILE "${SPARK_TEST_LOG}"
    TIMEOUT 180
)

if(NOT "${_result}" STREQUAL "0")
    set(_excerpt "<SparkTests produced no log>")
    if(EXISTS "${SPARK_TEST_LOG}")
        file(READ "${SPARK_TEST_LOG}" _excerpt LIMIT 65536)
    endif()
    message(FATAL_ERROR
        "SparkTests failed or could not be launched (result: ${_result}).\n"
        "Captured output (first 64 KiB):\n${_excerpt}")
endif()

if(NOT EXISTS "${SPARK_JUNIT_REPORT}")
    message(FATAL_ERROR "SparkTests returned success without producing JUnit: ${SPARK_JUNIT_REPORT}")
endif()
file(SIZE "${SPARK_JUNIT_REPORT}" _junit_size)
if(_junit_size EQUAL 0)
    message(FATAL_ERROR "SparkTests returned success with an empty JUnit report: ${SPARK_JUNIT_REPORT}")
endif()

message(STATUS "SparkTests completed successfully; JUnit bytes: ${_junit_size}")
