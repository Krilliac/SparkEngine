cmake_minimum_required(VERSION 3.25)

# Drives the real Linux SparkEngine host with -require-game and checks the
# post-teardown SPARK_MODULE_LIFECYCLE record and exit status on the SDL path
# (and, when SPARK_CHECK_HEADLESS is ON, the headless path):
#   1. SparkGame initializes      -> exit 0, one record with real callback counts
#   2. game module OnLoad fails   -> exit 2, one record showing load=0
#   3. -game names no image       -> exit 2, no record (nothing was created)
# The record format is the Windows host's wire contract parsed by
# tools/module-evidence/collect_lifecycle.py.

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_FAILING_MODULE SPARK_WORKING_DIRECTORY)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkLinuxModuleLifecycleRecord.cmake requires -D${_required}=<path>")
    endif()
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_FAILING_MODULE)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR "${_path} is missing: ${${_path}}")
    endif()
endforeach()

set(_record_prefix "SPARK_MODULE_LIFECYCLE module=")
set(_record_fields
    "create=([0-9]+) load=([0-9]+) update=([0-9]+) fixed=([0-9]+) render=([0-9]+) unload=([0-9]+) destroy=([0-9]+) faults=([0-9]+)")

# Run the engine; sets _exit, _stdout, _stderr in the caller's scope.
function(spark_run_engine)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "SPARK_RHI_BACKEND=null"
            "SDL_VIDEODRIVER=dummy"
            "SDL_AUDIODRIVER=dummy"
            ${ARGN}
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 60
        ENCODING UTF-8)
    string(REPLACE "\r\n" "\n" _out "${_out}")
    set(_exit "${_result}" PARENT_SCOPE)
    set(_stdout "${_out}" PARENT_SCOPE)
    set(_stderr "${_err}" PARENT_SCOPE)
endfunction()

# Require exactly one standalone record line for <module>; returns the counts
# as a list create;load;update;fixed;render;unload;destroy;faults.
function(spark_expect_one_record label module out_counts)
    string(REGEX MATCHALL "SPARK_MODULE_LIFECYCLE[^\n]*" _records "${_stdout}")
    list(LENGTH _records _record_count)
    if(NOT _record_count EQUAL 1)
        message(FATAL_ERROR "${label}: expected exactly one lifecycle record, found ${_record_count}.\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
    if(NOT _stdout MATCHES "(^|\n)${_record_prefix}${module} ${_record_fields}\n")
        message(FATAL_ERROR "${label}: malformed or wrong-module lifecycle record (want module=${module}): "
                            "${_records}\nstdout:\n${_stdout}")
    endif()
    set(${out_counts}
        "${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4};${CMAKE_MATCH_5};${CMAKE_MATCH_6};${CMAKE_MATCH_7};${CMAKE_MATCH_8};${CMAKE_MATCH_9}"
        PARENT_SCOPE)
endfunction()

function(spark_expect_exit label expected)
    if(NOT "${_exit}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: exited ${_exit}, expected ${expected}.\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endfunction()

function(spark_expect_counts label counts expected)
    if(NOT "${counts}" MATCHES "^${expected}$")
        message(FATAL_ERROR "${label}: lifecycle counts ${counts} (create;load;update;fixed;render;unload;destroy;"
                            "faults) do not match ${expected}.\nstdout:\n${_stdout}")
    endif()
endfunction()

get_filename_component(_game_target "${SPARK_GAME_MODULE}" NAME_WE)
string(REGEX REPLACE "^lib" "" _game_target "${_game_target}")
get_filename_component(_failing_target "${SPARK_FAILING_MODULE}" NAME_WE)
string(REGEX REPLACE "^lib" "" _failing_target "${_failing_target}")

set(_common_args -require-game -test-frames 3 -threads 1 -no-subprocess -minimal-init -no-jobsystem)

# 1. SDL path, healthy game module: nonzero per-frame counts, full teardown.
spark_run_engine("${SPARK_ENGINE_EXECUTABLE}" -game "${SPARK_GAME_MODULE}" ${_common_args})
spark_expect_exit("SDL healthy module" 0)
spark_expect_one_record("SDL healthy module" "${_game_target}" _counts)
spark_expect_counts("SDL healthy module" "${_counts}" "1;1;[1-9][0-9]*;[0-9]+;[1-9][0-9]*;1;1;0")

# 2. SDL path, game module whose OnLoad fails: the failure stays visible.
spark_run_engine("SPARK_MODULE_ABI_KIND_GAME=1" "SPARK_MODULE_ABI_FAIL_ON_LOAD=1"
                 "${SPARK_ENGINE_EXECUTABLE}" -game "${SPARK_FAILING_MODULE}" ${_common_args})
spark_expect_exit("SDL failing module" 2)
spark_expect_one_record("SDL failing module" "${_failing_target}" _counts)
spark_expect_counts("SDL failing module" "${_counts}" "1;0;0;0;0;[0-9]+;1;[0-9]+")

# 3. SDL path, -game names nothing loadable: exit 2 and no record.
spark_run_engine("${SPARK_ENGINE_EXECUTABLE}" -game "${SPARK_WORKING_DIRECTORY}/libSparkNoSuchModule.so"
                 ${_common_args})
spark_expect_exit("SDL missing module" 2)
if(_stdout MATCHES "SPARK_MODULE_LIFECYCLE")
    message(FATAL_ERROR "SDL missing module: printed a lifecycle record for a module that was never created.\n"
                        "stdout:\n${_stdout}")
endif()

if(SPARK_CHECK_HEADLESS)
    # The headless host skips module loading under -minimal-init, and keeps
    # module images mapped until process exit, so its record reports destroy=0.
    set(_common_args -require-game -test-frames 3 -threads 1 -no-subprocess -no-jobsystem)
    spark_run_engine("${SPARK_ENGINE_EXECUTABLE}" -headless -game "${SPARK_GAME_MODULE}" ${_common_args})
    spark_expect_exit("headless healthy module" 0)
    spark_expect_one_record("headless healthy module" "${_game_target}" _counts)
    spark_expect_counts("headless healthy module" "${_counts}" "1;1;[1-9][0-9]*;[0-9]+;[0-9]+;1;0;0")

    spark_run_engine("SPARK_MODULE_ABI_KIND_GAME=1" "SPARK_MODULE_ABI_FAIL_ON_LOAD=1"
                     "${SPARK_ENGINE_EXECUTABLE}" -headless -game "${SPARK_FAILING_MODULE}" ${_common_args})
    spark_expect_exit("headless failing module" 2)
    spark_expect_one_record("headless failing module" "${_failing_target}" _counts)
    spark_expect_counts("headless failing module" "${_counts}" "1;0;0;0;0;[0-9]+;1;[0-9]+")
endif()

message(STATUS "Linux host -require-game exit status and SPARK_MODULE_LIFECYCLE records verified")
