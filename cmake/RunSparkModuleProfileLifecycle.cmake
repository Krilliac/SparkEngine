cmake_minimum_required(VERSION 3.25)

function(_spark_validate_lifecycle_result child_result child_stdout child_stderr out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")

    if(NOT "${child_result}" STREQUAL "0")
        set(_ok FALSE)
        set(_reason "child exit status was ${child_result}, expected 0")
    else()
        set(_stdout "${child_stdout}")
        set(_stderr "${child_stderr}")
        string(REPLACE "\r\n" "\n" _stdout "${_stdout}")
        string(REPLACE "\r" "\n" _stdout "${_stdout}")
        string(REPLACE "\r\n" "\n" _stderr "${_stderr}")
        string(REPLACE "\r" "\n" _stderr "${_stderr}")

        # Require exactly one standalone record. Logger-prefixed copies are
        # deliberately excluded: only the post-teardown wWinMain write is
        # admissible evidence.
        set(_combined "${_stdout}\n${_stderr}")
        string(REPLACE ";" "\\;" _combined "${_combined}")
        string(REPLACE "\n" ";" _lines "${_combined}")
        set(_markers)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES
               "^SPARK_MODULE_LIFECYCLE initialized=[0-9]+ updated=[0-9]+ fixed=[0-9]+ rendered=[0-9]+ unloaded=[0-9]+ faults=[0-9]+$")
                list(APPEND _markers "${_line}")
            endif()
        endforeach()
        list(LENGTH _markers _marker_count)
        if(NOT _marker_count EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_marker_count} standalone lifecycle records, expected exactly 1")
        else()
            list(GET _markers 0 _marker)

            string(REGEX MATCH "initialized=([0-9]+)" _unused "${_marker}")
            set(_initialized "${CMAKE_MATCH_1}")
            string(REGEX MATCH "updated=([0-9]+)" _unused "${_marker}")
            set(_updated "${CMAKE_MATCH_1}")
            string(REGEX MATCH "fixed=([0-9]+)" _unused "${_marker}")
            set(_fixed "${CMAKE_MATCH_1}")
            string(REGEX MATCH "rendered=([0-9]+)" _unused "${_marker}")
            set(_rendered "${CMAKE_MATCH_1}")
            string(REGEX MATCH "unloaded=([0-9]+)" _unused "${_marker}")
            set(_unloaded "${CMAKE_MATCH_1}")
            string(REGEX MATCH "faults=([0-9]+)" _unused "${_marker}")
            set(_faults "${CMAKE_MATCH_1}")

            if(NOT "${_initialized}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "initialized callback count was ${_initialized}, expected exactly 1")
            elseif(NOT "${_unloaded}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "unloaded callback count was ${_unloaded}, expected exactly 1")
            elseif(_updated LESS 1)
                set(_ok FALSE)
                set(_reason "OnUpdate has no successful callback evidence")
            elseif(_fixed LESS 1)
                set(_ok FALSE)
                set(_reason "OnFixedUpdate has no successful callback evidence")
            elseif(_rendered LESS 1)
                set(_ok FALSE)
                set(_reason "OnRender has no successful callback evidence")
            elseif(NOT "${_faults}" STREQUAL "0")
                set(_ok FALSE)
                set(_reason "guarded module callback fault/disabled dispatch count was ${_faults}, expected 0")
            endif()
        endif()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_LIFECYCLE_PARSER_SELF_TEST)
    function(_spark_expect_lifecycle_case name result stdout stderr expected_ok)
        _spark_validate_lifecycle_result("${result}" "${stdout}" "${stderr}" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Lifecycle parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Lifecycle parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_valid "SPARK_MODULE_LIFECYCLE initialized=1 updated=4 fixed=2 rendered=4 unloaded=1 faults=0\n")
    _spark_expect_lifecycle_case(valid 0 "${_valid}" "" TRUE)
    _spark_expect_lifecycle_case(nonzero-exit 2 "${_valid}" "" FALSE)
    _spark_expect_lifecycle_case(logger-copy 0 "[info] ${_valid}" "" FALSE)
    _spark_expect_lifecycle_case(duplicate 0 "${_valid}${_valid}" "" FALSE)
    _spark_expect_lifecycle_case(wrong-initialized 0
        "SPARK_MODULE_LIFECYCLE initialized=2 updated=4 fixed=2 rendered=4 unloaded=1 faults=0\n" "" FALSE)
    _spark_expect_lifecycle_case(wrong-unloaded 0
        "SPARK_MODULE_LIFECYCLE initialized=1 updated=4 fixed=2 rendered=4 unloaded=0 faults=0\n" "" FALSE)
    _spark_expect_lifecycle_case(missing-update 0
        "SPARK_MODULE_LIFECYCLE initialized=1 updated=0 fixed=2 rendered=4 unloaded=1 faults=0\n" "" FALSE)
    _spark_expect_lifecycle_case(missing-fixed 0
        "SPARK_MODULE_LIFECYCLE initialized=1 updated=4 fixed=0 rendered=4 unloaded=1 faults=0\n" "" FALSE)
    _spark_expect_lifecycle_case(missing-render 0
        "SPARK_MODULE_LIFECYCLE initialized=1 updated=4 fixed=2 rendered=0 unloaded=1 faults=0\n" "" FALSE)
    _spark_expect_lifecycle_case(nonzero-faults 0
        "SPARK_MODULE_LIFECYCLE initialized=1 updated=4 fixed=2 rendered=4 unloaded=1 faults=1\n" "" FALSE)
    message(STATUS "SparkGameFPS D3D11 lifecycle parser contract passed")
    return()
endif()

foreach(_required
        SPARK_ENGINE_EXECUTABLE
        SPARK_GAME_MODULE
        SPARK_WORKING_DIRECTORY
        SPARK_RHI_BACKEND)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkModuleProfileLifecycle.cmake requires -D${_required}=<value>")
    endif()
endforeach()

if(NOT SPARK_RHI_BACKEND STREQUAL "d3d11")
    message(FATAL_ERROR
        "The stable-v1 source lifecycle smoke requires SPARK_RHI_BACKEND=d3d11")
endif()

if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPARK_GAME_MODULE}")
    message(FATAL_ERROR "Game module is missing: ${SPARK_GAME_MODULE}")
endif()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Lifecycle working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=${SPARK_RHI_BACKEND}"
        "${SPARK_ENGINE_EXECUTABLE}"
        -game "${SPARK_GAME_MODULE}"
        -require-game
        -test-seconds 1.0
        -threads 2
        -window-size 640x360
        -no-subprocess
    WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 90
    ENCODING UTF-8
)

_spark_validate_lifecycle_result("${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _lifecycle_reason)
if(NOT _lifecycle_ok)
    message(FATAL_ERROR
        "SparkGameFPS D3D11 production lifecycle evidence failed: ${_lifecycle_reason}.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

message(STATUS "SparkGameFPS D3D11 production lifecycle completed with all module phases evidenced")
