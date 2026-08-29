cmake_minimum_required(VERSION 3.25)

function(_spark_validate_headless_nullrhi_result child_result child_stdout child_stderr out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")

    if(NOT "${child_result}" STREQUAL "0")
        set(_ok FALSE)
        set(_reason "child exit status was ${child_result}, expected 0")
    else()
        set(_combined "${child_stdout}\n${child_stderr}")
        string(REPLACE "\r\n" "\n" _combined "${_combined}")
        string(REPLACE "\r" "\n" _combined "${_combined}")

        # Any actual D3D11 device record invalidates a no-render run, even if
        # the expected NullRHI records also happen to be present.
        string(REGEX MATCHALL "SPARK_D3D11_DEVICE[^\n]*" _rendered_device_records "${_combined}")
        list(LENGTH _rendered_device_records _rendered_device_count)
        if(NOT _rendered_device_count EQUAL 0)
            set(_ok FALSE)
            set(_reason "found ${_rendered_device_count} D3D11 device records in a NullRHI run")
        endif()

        string(REPLACE ";" "\\;" _combined "${_combined}")
        string(REPLACE "\n" ";" _lines "${_combined}")
        set(_module_ready_records)
        set(_rhi_records)
        set(_lifecycle_records)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^SPARK_MODULE_READY count=[0-9]+$")
                list(APPEND _module_ready_records "${_line}")
            elseif(_line MATCHES
                   "^SPARK_HEADLESS_RHI backend=null initialized=[0-9]+ frames=[0-9]+ shutdown=[0-9]+$")
                list(APPEND _rhi_records "${_line}")
            elseif(_line MATCHES
                   "^SPARK_HEADLESS_LIFECYCLE initialized=[0-9]+ updated=[0-9]+ fixed=[0-9]+ rendered=[0-9]+ unloaded=[0-9]+ faults=[0-9]+$")
                list(APPEND _lifecycle_records "${_line}")
            endif()
        endforeach()

        list(LENGTH _module_ready_records _module_ready_count)
        list(LENGTH _rhi_records _rhi_record_count)
        list(LENGTH _lifecycle_records _lifecycle_record_count)
        if(_ok AND NOT _module_ready_count EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_module_ready_count} standalone module-ready records, expected exactly 1")
        elseif(_ok AND NOT _rhi_record_count EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_rhi_record_count} standalone NullRHI records, expected exactly 1")
        elseif(_ok AND NOT _lifecycle_record_count EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_lifecycle_record_count} standalone headless lifecycle records, expected exactly 1")
        elseif(_ok)
            list(GET _module_ready_records 0 _module_ready)
            list(GET _rhi_records 0 _rhi)
            list(GET _lifecycle_records 0 _lifecycle)

            string(REGEX MATCH "count=([0-9]+)" _unused "${_module_ready}")
            set(_module_count "${CMAKE_MATCH_1}")
            string(REGEX MATCH "initialized=([0-9]+)" _unused "${_rhi}")
            set(_rhi_initialized "${CMAKE_MATCH_1}")
            string(REGEX MATCH "frames=([0-9]+)" _unused "${_rhi}")
            set(_rhi_frames "${CMAKE_MATCH_1}")
            string(REGEX MATCH "shutdown=([0-9]+)" _unused "${_rhi}")
            set(_rhi_shutdown "${CMAKE_MATCH_1}")

            string(REGEX MATCH "initialized=([0-9]+)" _unused "${_lifecycle}")
            set(_initialized "${CMAKE_MATCH_1}")
            string(REGEX MATCH "updated=([0-9]+)" _unused "${_lifecycle}")
            set(_updated "${CMAKE_MATCH_1}")
            string(REGEX MATCH "fixed=([0-9]+)" _unused "${_lifecycle}")
            set(_fixed "${CMAKE_MATCH_1}")
            string(REGEX MATCH "rendered=([0-9]+)" _unused "${_lifecycle}")
            set(_rendered "${CMAKE_MATCH_1}")
            string(REGEX MATCH "unloaded=([0-9]+)" _unused "${_lifecycle}")
            set(_unloaded "${CMAKE_MATCH_1}")
            string(REGEX MATCH "faults=([0-9]+)" _unused "${_lifecycle}")
            set(_faults "${CMAKE_MATCH_1}")

            if(NOT "${_module_count}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "initialized module count was ${_module_count}, expected exactly 1")
            elseif(NOT "${_rhi_initialized}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "NullRHI initialized flag was ${_rhi_initialized}, expected 1")
            elseif(_rhi_frames LESS 1)
                set(_ok FALSE)
                set(_reason "NullRHI completed no frames")
            elseif(NOT "${_rhi_shutdown}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "NullRHI shutdown flag was ${_rhi_shutdown}, expected 1")
            elseif(NOT "${_initialized}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "initialized callback count was ${_initialized}, expected exactly 1")
            elseif(_updated LESS 1)
                set(_ok FALSE)
                set(_reason "OnUpdate has no successful callback evidence")
            elseif(_fixed LESS 1)
                set(_ok FALSE)
                set(_reason "OnFixedUpdate has no successful callback evidence")
            elseif(NOT "${_rendered}" STREQUAL "0")
                set(_ok FALSE)
                set(_reason "OnRender callback count was ${_rendered}, expected 0")
            elseif(NOT "${_unloaded}" STREQUAL "1")
                set(_ok FALSE)
                set(_reason "unloaded callback count was ${_unloaded}, expected exactly 1")
            elseif(NOT "${_faults}" STREQUAL "0")
                set(_ok FALSE)
                set(_reason "guarded callback fault/disabled dispatch count was ${_faults}, expected 0")
            endif()
        endif()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_HEADLESS_NULLRHI_PARSER_SELF_TEST)
    function(_spark_expect_headless_case name result stdout stderr expected_ok)
        _spark_validate_headless_nullrhi_result("${result}" "${stdout}" "${stderr}" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Headless parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Headless parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_ready "SPARK_MODULE_READY count=1\n")
    set(_rhi "SPARK_HEADLESS_RHI backend=null initialized=1 frames=8 shutdown=1\n")
    set(_lifecycle
        "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=0 unloaded=1 faults=0\n")
    set(_valid "${_ready}${_rhi}${_lifecycle}")
    _spark_expect_headless_case(valid 0 "${_valid}" "" TRUE)
    _spark_expect_headless_case(nonzero-exit 2 "${_valid}" "" FALSE)
    _spark_expect_headless_case(missing-ready 0 "${_rhi}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(logger-only-ready 0 "[info] ${_ready}${_rhi}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(duplicate-ready 0 "${_ready}${_valid}" "" FALSE)
    _spark_expect_headless_case(wrong-module-count 0
        "SPARK_MODULE_READY count=2\n${_rhi}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(missing-rhi 0 "${_ready}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(duplicate-rhi 0 "${_ready}${_rhi}${_rhi}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(rhi-not-initialized 0
        "${_ready}SPARK_HEADLESS_RHI backend=null initialized=0 frames=8 shutdown=1\n${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(wrong-rhi-backend 0
        "${_ready}SPARK_HEADLESS_RHI backend=d3d11 initialized=1 frames=8 shutdown=1\n${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(zero-rhi-frames 0
        "${_ready}SPARK_HEADLESS_RHI backend=null initialized=1 frames=0 shutdown=1\n${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(rhi-not-shutdown 0
        "${_ready}SPARK_HEADLESS_RHI backend=null initialized=1 frames=8 shutdown=0\n${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(missing-lifecycle 0 "${_ready}${_rhi}" "" FALSE)
    _spark_expect_headless_case(duplicate-lifecycle 0 "${_valid}${_lifecycle}" "" FALSE)
    _spark_expect_headless_case(wrong-initialized 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=2 updated=8 fixed=7 rendered=0 unloaded=1 faults=0\n"
        "" FALSE)
    _spark_expect_headless_case(missing-update 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=0 fixed=7 rendered=0 unloaded=1 faults=0\n"
        "" FALSE)
    _spark_expect_headless_case(missing-fixed 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=0 rendered=0 unloaded=1 faults=0\n"
        "" FALSE)
    _spark_expect_headless_case(rendered 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=1 unloaded=1 faults=0\n"
        "" FALSE)
    _spark_expect_headless_case(wrong-unloaded 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=0 unloaded=0 faults=0\n"
        "" FALSE)
    _spark_expect_headless_case(nonzero-faults 0
        "${_ready}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=0 unloaded=1 faults=1\n"
        "" FALSE)
    _spark_expect_headless_case(d3d11-device 0
        "SPARK_D3D11_DEVICE driver=warp certification=software-only\n${_valid}" "" FALSE)
    message(STATUS "SparkGameFPS Windows NullRHI source-headless parser contract passed")
    return()
endif()

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_WORKING_DIRECTORY SPARK_RHI_BACKEND)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkHeadlessNullRHILifecycle.cmake requires -D${_required}=<value>")
    endif()
endforeach()

if(NOT SPARK_RHI_BACKEND STREQUAL "null")
    message(FATAL_ERROR "The Windows source-headless lifecycle gate requires SPARK_RHI_BACKEND=null")
endif()
if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPARK_GAME_MODULE}")
    message(FATAL_ERROR "SparkGameFPS module is missing: ${SPARK_GAME_MODULE}")
endif()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Headless working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=${SPARK_RHI_BACKEND}"
        "${SPARK_ENGINE_EXECUTABLE}"
        -headless
        -game "${SPARK_GAME_MODULE}"
        -require-game
        -test-frames 8
        -threads 2
        -no-subprocess
    WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 90
    ENCODING UTF-8
)

_spark_validate_headless_nullrhi_result("${_result}" "${_stdout}" "${_stderr}" _ok _reason)
if(NOT _ok)
    message(FATAL_ERROR
        "SparkGameFPS Windows NullRHI source-headless evidence failed: ${_reason}.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

message(STATUS "SparkGameFPS Windows NullRHI source-headless lifecycle completed with exact evidence")
