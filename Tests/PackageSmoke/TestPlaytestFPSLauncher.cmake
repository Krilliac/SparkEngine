# Exercise the same entry point shipped to Windows playtesters. The installed
# package owns the binary and DLL; no source-tree fallback is allowed.
if(NOT DEFINED SPARK_INSTALLED_ROOT OR NOT IS_ABSOLUTE "${SPARK_INSTALLED_ROOT}")
    message(FATAL_ERROR "SPARK_INSTALLED_ROOT must be an absolute installed package root")
endif()
if(NOT DEFINED SPARK_PLAYTEST_TEST_ROOT OR NOT IS_ABSOLUTE "${SPARK_PLAYTEST_TEST_ROOT}")
    message(FATAL_ERROR "SPARK_PLAYTEST_TEST_ROOT must be an absolute isolated test root")
endif()
file(MAKE_DIRECTORY "${SPARK_PLAYTEST_TEST_ROOT}/localappdata")

set(_bin "${SPARK_INSTALLED_ROOT}/bin")
set(_launcher "${_bin}/PlaytestSparkFPS.cmd")
if(NOT EXISTS "${_launcher}" OR IS_DIRECTORY "${_launcher}")
    message(FATAL_ERROR "Installed playtester launcher is missing: ${_launcher}")
endif()

execute_process(
    COMMAND cmd /d /c "${_launcher}" check
    WORKING_DIRECTORY "${_bin}"
    RESULT_VARIABLE _check_result
    OUTPUT_VARIABLE _check_output
    ERROR_VARIABLE _check_error
    TIMEOUT 30)
if(NOT "${_check_result}" STREQUAL "0" OR
   NOT _check_output MATCHES "SparkEngine [0-9]+[.][0-9]+[.][0-9]+")
    message(FATAL_ERROR "Installed launcher check failed: ${_check_result}\n${_check_output}\n${_check_error}")
endif()

execute_process(
    COMMAND cmd /d /c "${_launcher}" report-url
    WORKING_DIRECTORY "${_bin}"
    RESULT_VARIABLE _url_result
    OUTPUT_VARIABLE _url_output
    ERROR_VARIABLE _url_error
    TIMEOUT 10)
string(STRIP "${_url_output}" _url_output)
if(NOT "${_url_result}" STREQUAL "0" OR
   NOT _url_output STREQUAL "https://github.com/Krilliac/SparkEngine/issues/new?template=playtest_bug.md")
    message(FATAL_ERROR "Installed launcher report URL is not the fixed repo issue form: ${_url_result}\n${_url_output}\n${_url_error}")
endif()

execute_process(
    COMMAND cmd /d /c "${_launcher}" invalid-mode
    WORKING_DIRECTORY "${_bin}"
    RESULT_VARIABLE _invalid_result
    OUTPUT_VARIABLE _invalid_output
    ERROR_VARIABLE _invalid_error
    TIMEOUT 10)
if("${_invalid_result}" STREQUAL "0")
    message(FATAL_ERROR "Installed launcher accepted an unsupported mode")
endif()

set(_missing_module_dir "${SPARK_PLAYTEST_TEST_ROOT}/missing-module")
file(MAKE_DIRECTORY "${_missing_module_dir}")
file(COPY_FILE "${_launcher}" "${_missing_module_dir}/PlaytestSparkFPS.cmd")
file(WRITE "${_missing_module_dir}/SparkEngine.exe" "fixture only\n")
execute_process(
    COMMAND cmd /d /c "${_missing_module_dir}/PlaytestSparkFPS.cmd" check
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_output
    ERROR_VARIABLE _missing_error
    TIMEOUT 10)
if("${_missing_result}" STREQUAL "0" OR
   NOT _missing_output MATCHES "SparkGameFPS[.]dll is missing")
    message(FATAL_ERROR "Launcher did not reject a missing installed FPS module: ${_missing_result}\n${_missing_output}\n${_missing_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LOCALAPPDATA=${SPARK_PLAYTEST_TEST_ROOT}/localappdata"
        cmd /d /c "${_launcher}" smoke
    WORKING_DIRECTORY "${_bin}"
    RESULT_VARIABLE _smoke_result
    OUTPUT_VARIABLE _smoke_output
    ERROR_VARIABLE _smoke_error
    TIMEOUT 90)
set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/RunSparkHeadlessNullRHILifecycle.cmake")
_spark_validate_headless_nullrhi_result(
    "${_smoke_result}" "${_smoke_output}" "${_smoke_error}" _smoke_ok _smoke_reason)
if(NOT _smoke_ok)
    message(FATAL_ERROR "Installed playtester NullRHI smoke failed: ${_smoke_reason}\nstdout:\n${_smoke_output}\nstderr:\n${_smoke_error}")
endif()

message(STATUS "Installed PlaytestSparkFPS check, report URL, invalid mode, and real NullRHI smoke passed")
