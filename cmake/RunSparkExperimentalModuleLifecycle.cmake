cmake_minimum_required(VERSION 3.25)

# RDY-015: real-source lifecycle evidence for one experimental game module.
#
# Launches the production Linux SparkEngine host (SDL path, NullRHI, dummy SDL
# drivers) with -require-game against one experimental module image and parses
# the host's post-teardown record:
#
#   SPARK_MODULE_LIFECYCLE module=<target> create= load= update= fixed= render= unload= destroy= faults=
#
# The run passes only when the host exits 0, prints exactly one standalone
# SPARK_MODULE_READY count=1 line followed by exactly one standalone record
# naming this module, every lifecycle phase ran at least once, and no guarded
# callback faulted. The record format is the Windows host's wire contract
# parsed by tools/module-evidence/collect_lifecycle.py.
#
# This is prototype evidence only. It never feeds the stable-v1 profile.
#
# Modes:
#   -DSPARK_EXPERIMENTAL_LIFECYCLE_PARSER_SELF_TEST=ON   parser contract only
#   -DSPARK_ENGINE_EXECUTABLE=... -DSPARK_GAME_MODULE=... -DSPARK_MODULE_TARGET=...
#   -DSPARK_WORKING_DIRECTORY=...                         live run

set(_spark_lifecycle_phases create load update fixed render unload destroy)

function(_spark_validate_experimental_lifecycle child_result child_stdout module out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")

    if(NOT "${child_result}" STREQUAL "0")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "host exit status was ${child_result}, expected 0" PARENT_SCOPE)
        return()
    endif()

    set(_text "${child_stdout}")
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(REPLACE "\r" "\n" _text "${_text}")
    string(REPLACE ";" "\\;" _text "${_text}")
    # CMake list splitting does not break inside unbalanced square brackets, so
    # one host/log line with a stray '[' or ']' would merge every later line
    # into one element and hide (or fabricate the absence of) evidence records.
    # Neither marker contains brackets, so neutralizing them is lossless for
    # validation.
    string(REPLACE "[" "<" _text "${_text}")
    string(REPLACE "]" ">" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")

    # Only standalone lines count: a logger-prefixed echo of either marker is
    # diagnostic text, not the host's evidence record.
    set(_ready_records)
    set(_lifecycle_records)
    set(_marker_order "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^SPARK_MODULE_READY count=[0-9]+$")
            list(APPEND _ready_records "${_line}")
            string(APPEND _marker_order "R")
        elseif(_line MATCHES "^SPARK_MODULE_LIFECYCLE ")
            list(APPEND _lifecycle_records "${_line}")
            string(APPEND _marker_order "L")
        endif()
    endforeach()

    list(LENGTH _ready_records _ready_count)
    list(LENGTH _lifecycle_records _lifecycle_count)
    if(NOT _ready_count EQUAL 1)
        set(_ok FALSE)
        set(_reason "found ${_ready_count} standalone SPARK_MODULE_READY records, expected exactly 1")
    elseif(NOT _lifecycle_count EQUAL 1)
        set(_ok FALSE)
        set(_reason "found ${_lifecycle_count} standalone SPARK_MODULE_LIFECYCLE records, expected exactly 1")
    elseif(NOT _marker_order STREQUAL "RL")
        set(_ok FALSE)
        set(_reason "the lifecycle record was printed before the module-ready record")
    else()
        list(GET _ready_records 0 _ready)
        list(GET _lifecycle_records 0 _record)
        set(_record_pattern "^SPARK_MODULE_LIFECYCLE module=([^ ]+) create=([0-9]+) load=([0-9]+) ")
        string(APPEND _record_pattern "update=([0-9]+) fixed=([0-9]+) render=([0-9]+) unload=([0-9]+) ")
        string(APPEND _record_pattern "destroy=([0-9]+) faults=([0-9]+)$")
        if(NOT _ready STREQUAL "SPARK_MODULE_READY count=1")
            set(_ok FALSE)
            set(_reason "module-ready record '${_ready}' does not report exactly one initialized module")
        elseif(NOT _record MATCHES "${_record_pattern}")
            set(_ok FALSE)
            set(_reason "malformed lifecycle record: ${_record}")
        else()
            set(_record_module "${CMAKE_MATCH_1}")
            set(_counts "${CMAKE_MATCH_2};${CMAKE_MATCH_3};${CMAKE_MATCH_4};${CMAKE_MATCH_5}")
            list(APPEND _counts "${CMAKE_MATCH_6}" "${CMAKE_MATCH_7}" "${CMAKE_MATCH_8}")
            set(_faults "${CMAKE_MATCH_9}")
            if(NOT _record_module STREQUAL module)
                set(_ok FALSE)
                set(_reason "lifecycle record names module '${_record_module}', expected '${module}'")
            elseif(NOT _faults STREQUAL "0")
                set(_ok FALSE)
                set(_reason "guarded callback fault/disabled dispatch count was ${_faults}, expected 0")
            else()
                foreach(_phase _count IN ZIP_LISTS _spark_lifecycle_phases _counts)
                    if(_count LESS 1)
                        set(_ok FALSE)
                        set(_reason "lifecycle phase '${_phase}' ran ${_count} times, expected at least 1")
                        break()
                    endif()
                endforeach()
            endif()
        endif()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_EXPERIMENTAL_LIFECYCLE_PARSER_SELF_TEST)
    function(_spark_expect_case name result stdout expected_ok)
        _spark_validate_experimental_lifecycle("${result}" "${stdout}" "SparkGameRTS" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Experimental lifecycle parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Experimental lifecycle parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_ready "SPARK_MODULE_READY count=1\n")
    set(_prefix "SPARK_MODULE_LIFECYCLE module=SparkGameRTS")
    set(_counts "create=1 load=1 update=3 fixed=3 render=3 unload=1 destroy=1")
    set(_record "${_prefix} ${_counts} faults=0\n")
    set(_valid "log line\n${_ready}more log\n${_record}")

    _spark_expect_case(valid 0 "${_valid}" TRUE)
    _spark_expect_case(valid-crlf 0 "SPARK_MODULE_READY count=1\r\n${_prefix} ${_counts} faults=0\r\n" TRUE)
    _spark_expect_case(nonzero-exit 2 "${_valid}" FALSE)
    _spark_expect_case(timeout-exit "Process terminated due to timeout" "${_valid}" FALSE)
    _spark_expect_case(missing-ready 0 "${_record}" FALSE)
    _spark_expect_case(logger-only-ready 0 "[INFO] ${_ready}${_record}" FALSE)
    _spark_expect_case(duplicate-ready 0 "${_ready}${_valid}" FALSE)
    _spark_expect_case(two-modules-ready 0 "SPARK_MODULE_READY count=2\n${_record}" FALSE)
    _spark_expect_case(missing-record 0 "${_ready}" FALSE)
    _spark_expect_case(logger-only-record 0 "${_ready}[INFO] ${_record}" FALSE)
    _spark_expect_case(duplicate-record 0 "${_valid}${_record}" FALSE)
    _spark_expect_case(unbalanced-open-bracket 0 "log [unbalanced\n${_ready}${_record}" TRUE)
    _spark_expect_case(unbalanced-close-bracket 0 "log ]unbalanced\n${_ready}${_record}" TRUE)
    _spark_expect_case(bracket-hides-duplicate-record 0 "${_valid}log [unbalanced\n${_record}" FALSE)
    _spark_expect_case(bracket-hides-duplicate-ready 0 "${_ready}log [open\n${_valid}" FALSE)
    _spark_expect_case(record-before-ready 0 "${_record}${_ready}" FALSE)
    _spark_expect_case(wrong-module 0
        "${_ready}SPARK_MODULE_LIFECYCLE module=SparkGameFPS ${_counts} faults=0\n"
        FALSE)
    _spark_expect_case(prefixed-module 0
        "${_ready}SPARK_MODULE_LIFECYCLE module=libSparkGameRTS ${_counts} faults=0\n"
        FALSE)
    _spark_expect_case(missing-field 0
        "${_ready}${_prefix} create=1 load=1 update=3 render=3 unload=1 destroy=1 faults=0\n" FALSE)
    _spark_expect_case(trailing-garbage 0
        "${_ready}${_prefix} ${_counts} faults=0 extra=1\n" FALSE)
    _spark_expect_case(nonzero-faults 0
        "${_ready}${_prefix} ${_counts} faults=1\n" FALSE)
    set(_zero_phase_cases
        "create=0 load=1 update=3 fixed=3 render=3 unload=1 destroy=1"
        "create=1 load=0 update=3 fixed=3 render=3 unload=1 destroy=1"
        "create=1 load=1 update=0 fixed=3 render=3 unload=1 destroy=1"
        "create=1 load=1 update=3 fixed=0 render=3 unload=1 destroy=1"
        "create=1 load=1 update=3 fixed=3 render=0 unload=1 destroy=1"
        "create=1 load=1 update=3 fixed=3 render=3 unload=0 destroy=1"
        "create=1 load=1 update=3 fixed=3 render=3 unload=1 destroy=0")
    foreach(_phase _counts IN ZIP_LISTS _spark_lifecycle_phases _zero_phase_cases)
        _spark_expect_case("zero-${_phase}" 0 "${_ready}${_prefix} ${_counts} faults=0\n" FALSE)
    endforeach()
    # The shape the Linux host prints when a module's OnLoad fails under
    # -require-game (exit 2, load=0): both signals must fail the run.
    _spark_expect_case(onload-failure 2
        "${_prefix} create=1 load=0 update=0 fixed=0 render=0 unload=1 destroy=1 faults=0\n" FALSE)

    message(STATUS "Experimental module lifecycle parser contract passed")
    return()
endif()

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_MODULE_TARGET SPARK_WORKING_DIRECTORY)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkExperimentalModuleLifecycle.cmake requires -D${_required}=<value>")
    endif()
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_WORKING_DIRECTORY)
    # The host runs in SPARK_WORKING_DIRECTORY, so resolve relative inputs now.
    get_filename_component(${_path} "${${_path}}" ABSOLUTE)
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR "${_path} is missing: ${${_path}}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "SPARK_WORKING_DIRECTORY is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

# The host names the record after the loaded library, so the image and the
# expected target must agree before the run can prove anything about it.
get_filename_component(_image_stem "${SPARK_GAME_MODULE}" NAME_WE)
string(REGEX REPLACE "^lib" "" _image_stem "${_image_stem}")
if(NOT _image_stem STREQUAL SPARK_MODULE_TARGET)
    message(FATAL_ERROR "Module image ${SPARK_GAME_MODULE} does not belong to target ${SPARK_MODULE_TARGET}")
endif()

# The SDL host drives OnFixedUpdate from a wall-clock accumulator (60 Hz) while
# NullRHI frames take microseconds, so a short run can finish before one fixed
# step elapses. 120000 frames keep the loop running for roughly a second on a
# fast host (about 50 fixed steps); slower hosts only accumulate more.
set(_test_frames 120000)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=null"
        "SDL_VIDEODRIVER=dummy"
        "SDL_AUDIODRIVER=dummy"
        "${SPARK_ENGINE_EXECUTABLE}"
        -game "${SPARK_GAME_MODULE}"
        -require-game
        -test-frames ${_test_frames}
        -threads 1
        -no-subprocess
        -minimal-init
        -no-jobsystem
    WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 50
    ENCODING UTF-8)

_spark_validate_experimental_lifecycle("${_result}" "${_stdout}" "${SPARK_MODULE_TARGET}" _ok _reason)
if(NOT _ok)
    message(FATAL_ERROR
        "${SPARK_MODULE_TARGET} experimental lifecycle evidence failed: ${_reason}.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

string(REGEX MATCH "SPARK_MODULE_LIFECYCLE [^\n]*" _record_line "${_stdout}")
message(STATUS "${_record_line}")
message(STATUS "${SPARK_MODULE_TARGET} experimental (prototype) lifecycle completed on the Linux SDL host")
