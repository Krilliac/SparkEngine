cmake_minimum_required(VERSION 3.25)

# HEAD-220: forced-termination recovery of the INSTALLED headless FPS host
# (Windows). The helper Tests/PackageSmoke/forced_kill_recovery.py starts the
# installed SparkEngine.exe on NullRHI, TerminateProcess-es it after its
# quicksave, and requires the next boot on the same user-data tree to be clean:
# the saved state is restored, the strict NullRHI lifecycle records hold and
# SPARK_HEADLESS_NULLRHI_RESOURCES reports live=0.
#
# Inputs: SPARK_INSTALLED_ROOT (a staged runtime prefix that already passed the
# package validator; Tests/PackageSmoke/RunInstalledFPSPackage.cmake stages and
# validates it in SPARK_FPS_PACKAGE_MODE=forced-kill-recovery), SPARK_SOURCE_ROOT
# and SPARK_TEST_ROOT (absent or empty).
# SPARK_FORCED_KILL_PARSER_SELF_TEST=ON (SPARK_SOURCE_ROOT optional) runs the helper's validators on canned
# host output with no engine, so it works without a build or on any host.

# The self-test needs no inputs: the repository root defaults to this script's checkout.
if(NOT DEFINED SPARK_SOURCE_ROOT OR "${SPARK_SOURCE_ROOT}" STREQUAL "")
    cmake_path(SET SPARK_SOURCE_ROOT NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../..")
endif()
set(_helper "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/forced_kill_recovery.py")
if(NOT EXISTS "${_helper}")
    message(FATAL_ERROR "Forced-kill recovery helper is missing: ${_helper}")
endif()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)

if(SPARK_FORCED_KILL_PARSER_SELF_TEST)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_helper}" --self-test
        RESULT_VARIABLE _self_test_result
        OUTPUT_VARIABLE _self_test_output
        ERROR_VARIABLE _self_test_error
        TIMEOUT 60)
    if(NOT "${_self_test_result}" STREQUAL "0" OR
       NOT _self_test_output MATCHES "forced_kill_recovery: parser self-test passed")
        message(FATAL_ERROR
            "Forced-kill recovery parser self-test failed (${_self_test_result}):\n"
            "${_self_test_output}\n${_self_test_error}")
    endif()
    message(STATUS "Forced-kill recovery parser contract passed")
    return()
endif()

if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "Forced-kill recovery of the installed host runs on Windows only (TerminateProcess)")
endif()
foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunInstalledFPSForcedKillRecovery.cmake requires -D${_required}=<value>")
    endif()
    if(NOT IS_ABSOLUTE "${${_required}}" OR "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()

set(_bin "${SPARK_INSTALLED_ROOT}/bin")
foreach(_image IN ITEMS "${_bin}/SparkEngine.exe" "${_bin}/SparkGameFPS.dll")
    if(NOT EXISTS "${_image}" OR IS_DIRECTORY "${_image}" OR IS_SYMLINK "${_image}")
        message(FATAL_ERROR "Installed forced-kill input is missing or unsafe: ${_image}")
    endif()
endforeach()
if(EXISTS "${SPARK_TEST_ROOT}" OR IS_SYMLINK "${SPARK_TEST_ROOT}")
    message(FATAL_ERROR "Forced-kill test root must be absent before the run: ${SPARK_TEST_ROOT}")
endif()
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")

# SPARK_FPS_DEVELOPER_COMMANDS (default ON): plain Shipping registers no `xp`, so the helper
# expects the writer's `xp 37` to be refused and the restart to restore 0 XP.
if(NOT DEFINED SPARK_FPS_DEVELOPER_COMMANDS)
    set(SPARK_FPS_DEVELOPER_COMMANDS ON)
endif()
string(TOUPPER "${SPARK_FPS_DEVELOPER_COMMANDS}" _developer_commands)
if(_developer_commands MATCHES "^(ON|TRUE|1)$")
    set(_developer_commands ON)
elseif(_developer_commands MATCHES "^(OFF|FALSE|0)$")
    set(_developer_commands OFF)
else()
    message(FATAL_ERROR
        "SPARK_FPS_DEVELOPER_COMMANDS must be ON, OFF, TRUE, FALSE, 1 or 0, got '${SPARK_FPS_DEVELOPER_COMMANDS}'")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${_helper}"
        --engine "${_bin}/SparkEngine.exe"
        --module "${_bin}/SparkGameFPS.dll"
        --source-root "${SPARK_SOURCE_ROOT}"
        --cmake "${CMAKE_COMMAND}"
        --developer-commands "${_developer_commands}"
        --workdir "${SPARK_TEST_ROOT}/run"
    RESULT_VARIABLE _kill_result
    OUTPUT_VARIABLE _kill_output
    ERROR_VARIABLE _kill_error
    TIMEOUT 420)
if(NOT "${_kill_result}" STREQUAL "0" OR NOT _kill_output MATCHES "forced_kill_recovery: PASS")
    message(FATAL_ERROR
        "Installed FPS forced-kill recovery failed (${_kill_result}):\n${_kill_output}\n${_kill_error}\n"
        "Run logs: ${SPARK_TEST_ROOT}/run")
endif()
string(STRIP "${_kill_output}" _kill_output)
message(STATUS "${_kill_output}")
message(STATUS "Installed FPS forced-kill recovery passed; evidence retained under ${SPARK_TEST_ROOT}/run")
