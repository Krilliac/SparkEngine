cmake_minimum_required(VERSION 3.25)

# HEAD-220: source-tree NullRHI save/reload persistence of the production
# headless FPS host. Two fresh `-headless` processes share one isolated user
# root: the writer awards 37 XP and quicksaves, the reader proves it starts from
# 0 XP, quickloads and reads 37 XP back. Each process must pass the strict
# NullRHI lifecycle parser (one module, rendered=0, faults=0, no D3D11 device)
# and the same writer/reader audit validator as the installed-package WARP run
# (Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake). The reader must leave
# the save byte-identical. Source-tree evidence only, not packaged
# certification.

set(SPARK_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SPARK_FPS_SAVE_RELOAD_PARSER_ONLY ON)
include("${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake")
unset(SPARK_FPS_SAVE_RELOAD_PARSER_ONLY)
set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/RunSparkHeadlessNullRHILifecycle.cmake")
unset(SPARK_HEADLESS_NULLRHI_PARSER_ONLY)

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_WORKING_DIRECTORY SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkHeadlessFPSSaveReload.cmake requires -D${_required}=<value>")
    endif()
endforeach()
if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPARK_GAME_MODULE}")
    message(FATAL_ERROR "SparkGameFPS module is missing: ${SPARK_GAME_MODULE}")
endif()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Headless working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()
if(NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR "${SPARK_TEST_ROOT}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()

# A fresh user root per run: the save path comes from Spark::UserPaths, so a
# save left by an earlier run (or the developer's own profile) cannot be read.
file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
set(_user_root "${SPARK_TEST_ROOT}/user")
if(CMAKE_HOST_WIN32)
    set(_user_env
        "LOCALAPPDATA=${_user_root}/localappdata"
        "APPDATA=${_user_root}/appdata")
    set(_save_dir "${_user_root}/localappdata/SparkEngine/Saves")
else()
    set(_user_env
        "HOME=${_user_root}/home"
        "XDG_DATA_HOME=${_user_root}/data"
        "XDG_CONFIG_HOME=${_user_root}/config"
        "XDG_CACHE_HOME=${_user_root}/cache"
        "XDG_STATE_HOME=${_user_root}/state")
    set(_save_dir "${_user_root}/data/SparkEngine/Saves")
endif()
foreach(_assignment IN LISTS _user_env)
    string(REGEX REPLACE "^[A-Z_]+=" "" _dir "${_assignment}")
    file(MAKE_DIRECTORY "${_dir}")
endforeach()
set(_save "${_save_dir}/fps_quicksave.spark_save")

set(_writer_script "${SPARK_TEST_ROOT}/writer-exec.txt")
set(_reader_script "${SPARK_TEST_ROOT}/reader-exec.txt")
file(WRITE "${_writer_script}" "1 level\n2 xp 37\n3 level\n4 quicksave\n")
file(WRITE "${_reader_script}" "1 level\n2 quickload\n3 level\n")

function(_spark_run_headless_fps_phase phase script)
    set(_audit "${SPARK_TEST_ROOT}/${phase}-exec-audit.log")
    if(EXISTS "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Could not establish a fresh exec audit before the ${phase} run")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_user_env}
            "SPARK_RHI_BACKEND=null"
            "${SPARK_ENGINE_EXECUTABLE}"
            -headless
            -game "${SPARK_GAME_MODULE}"
            -require-game
            -exec "${script}"
            -exec-audit "${_audit}"
            -test-frames 30
            -threads 2
            -no-subprocess
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT 120
        ENCODING UTF-8)
    file(WRITE "${SPARK_TEST_ROOT}/${phase}-stdout.log" "${_stdout}")
    file(WRITE "${SPARK_TEST_ROOT}/${phase}-stderr.log" "${_stderr}")

    _spark_validate_headless_nullrhi_result("${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _lifecycle_reason)
    if(NOT _lifecycle_ok)
        message(FATAL_ERROR
            "Headless FPS ${phase} NullRHI lifecycle failed: ${_lifecycle_reason}\n"
            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()

    if(NOT EXISTS "${_audit}" OR IS_DIRECTORY "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Headless FPS ${phase} run did not write a regular exec audit: ${_audit}")
    endif()
    file(READ "${_audit}" _audit_content)
    _spark_validate_fps_audit("${phase}" "${_result}" TRUE "${_audit_content}" _audit_ok _audit_reason)
    if(NOT _audit_ok)
        message(FATAL_ERROR
            "Headless FPS ${phase} exec audit failed: ${_audit_reason}\n"
            "audit (${_audit}):\n${_audit_content}")
    endif()
endfunction()

_spark_run_headless_fps_phase(writer "${_writer_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Headless FPS writer did not create a regular quicksave under the isolated user root: ${_save}")
endif()
file(SIZE "${_save}" _save_size)
if(_save_size LESS 1 OR _save_size GREATER 67108864)
    message(FATAL_ERROR "Headless FPS quicksave size ${_save_size} is invalid")
endif()
file(SHA256 "${_save}" _writer_save_sha256)

_spark_run_headless_fps_phase(reader "${_reader_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Headless FPS reader lost the quicksave: ${_save}")
endif()
file(SHA256 "${_save}" _reader_save_sha256)
if(NOT _writer_save_sha256 STREQUAL _reader_save_sha256)
    message(FATAL_ERROR
        "Headless FPS reader mutated the persisted quicksave:\n"
        "  before: ${_writer_save_sha256}\n  after:  ${_reader_save_sha256}")
endif()

message(STATUS
    "Headless FPS NullRHI save/reload passed across two fresh processes "
    "(${_save_size}-byte save ${_writer_save_sha256}, ${CMAKE_HOST_SYSTEM_NAME} host)")
