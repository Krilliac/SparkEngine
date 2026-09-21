cmake_minimum_required(VERSION 3.25)

set(_runner_script "${CMAKE_CURRENT_LIST_FILE}")

if(NOT DEFINED SPARK_SOURCE_ROOT OR "${SPARK_SOURCE_ROOT}" STREQUAL "")
    message(FATAL_ERROR "SPARK_SOURCE_ROOT is required")
endif()

set(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY ON)
include("${SPARK_SOURCE_ROOT}/cmake/RunSparkModuleProfileLifecycle.cmake")
unset(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY)

function(_spark_count_exact_line lines_var expected out_count)
    set(_count 0)
    foreach(_line IN LISTS ${lines_var})
        if("${_line}" STREQUAL "${expected}")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    set(${out_count} "${_count}" PARENT_SCOPE)
endfunction()

function(_spark_validate_progression_lines lines_var expected_xp out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    _spark_count_exact_line(${lines_var} "    > === Progression ===" _heading_count)
    if(NOT _heading_count EQUAL 1)
        set(_ok FALSE)
        set(_reason "progression heading count was ${_heading_count}, expected 1")
    endif()

    set(_level_lines)
    set(_xp_lines)
    foreach(_line IN LISTS ${lines_var})
        if(_line MATCHES "^Level: ")
            list(APPEND _level_lines "${_line}")
        elseif(_line MATCHES "^XP: ")
            list(APPEND _xp_lines "${_line}")
        endif()
    endforeach()
    list(LENGTH _level_lines _level_count)
    list(LENGTH _xp_lines _xp_count)
    if(_ok AND NOT _level_count EQUAL 1)
        set(_ok FALSE)
        set(_reason "progression level-line count was ${_level_count}, expected 1")
    elseif(_ok AND NOT _xp_count EQUAL 1)
        set(_ok FALSE)
        set(_reason "progression XP-line count was ${_xp_count}, expected 1")
    elseif(_ok)
        list(GET _level_lines 0 _level_line)
        list(GET _xp_lines 0 _xp_line)
        if(NOT _level_line STREQUAL "Level: 1/50")
            set(_ok FALSE)
            set(_reason "unexpected progression level line '${_level_line}'")
        elseif(NOT _xp_line STREQUAL "XP: ${expected_xp}/282")
            set(_ok FALSE)
            set(_reason "unexpected progression XP line '${_xp_line}'")
        endif()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

function(_spark_validate_fps_audit phase child_result audit_fresh audit out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")

    if(NOT "${child_result}" STREQUAL "0")
        set(_ok FALSE)
        set(_reason "child exit status was ${child_result}, expected 0")
    elseif(NOT audit_fresh)
        set(_ok FALSE)
        set(_reason "audit freshness was not established before launch")
    elseif("${audit}" STREQUAL "")
        set(_ok FALSE)
        set(_reason "audit is empty")
    endif()

    if(_ok)
        set(_normalized "${audit}")
        string(REPLACE "\r\n" "\n" _normalized "${_normalized}")
        string(REPLACE "\r" "\n" _normalized "${_normalized}")
        string(REPLACE ";" "\\;" _escaped "${_normalized}")
        string(REPLACE "\n" ";" _lines "${_escaped}")

        if(phase STREQUAL "writer")
            set(_expected_commands "level" "xp 37" "level" "quicksave")
        elseif(phase STREQUAL "reader")
            set(_expected_commands "level" "quickload" "level")
        else()
            set(_ok FALSE)
            set(_reason "unknown audit phase '${phase}'")
        endif()
    endif()

    if(_ok)
        set(_header_indices)
        set(_line_index 0)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^frame ([0-9]+) t=([0-9]+\\.[0-9])s \\| (ok |ERR) \\| (.+)$")
                list(APPEND _header_indices "${_line_index}")
            elseif(_line MATCHES "^frame ")
                set(_ok FALSE)
                set(_reason "malformed audit header '${_line}'")
                break()
            endif()
            math(EXPR _line_index "${_line_index} + 1")
        endforeach()
        list(LENGTH _header_indices _header_count)
        list(LENGTH _expected_commands _expected_count)
        if(_ok AND NOT _header_count EQUAL _expected_count)
            set(_ok FALSE)
            set(_reason "audit header count was ${_header_count}, expected ${_expected_count}")
        endif()
    endif()

    if(_ok)
        list(LENGTH _lines _line_count)
        math(EXPR _last_header_position "${_header_count} - 1")
        foreach(_header_position RANGE 0 ${_last_header_position})
            list(GET _header_indices ${_header_position} _header_index)
            list(GET _lines ${_header_index} _header)
            list(GET _expected_commands ${_header_position} _expected_command)
            if(NOT _header MATCHES "^frame ([0-9]+) t=([0-9]+\\.[0-9])s \\| (ok |ERR) \\| (.+)$")
                set(_ok FALSE)
                set(_reason "audit header changed during parsing")
                break()
            endif()
            set(_frame "${CMAKE_MATCH_1}")
            set(_time "${CMAKE_MATCH_2}")
            set(_status "${CMAKE_MATCH_3}")
            set(_command "${CMAKE_MATCH_4}")
            if(NOT _status STREQUAL "ok ")
                set(_ok FALSE)
                set(_reason "command '${_command}' did not have an ok dispatch header")
                break()
            elseif(NOT _command STREQUAL _expected_command)
                set(_ok FALSE)
                set(_reason "command ${_header_position} was '${_command}', expected '${_expected_command}'")
                break()
            endif()

            math(EXPR _block_start "${_header_index} + 1")
            if(_header_position LESS _last_header_position)
                math(EXPR _next_position "${_header_position} + 1")
                list(GET _header_indices ${_next_position} _block_end)
            else()
                set(_block_end "${_line_count}")
            endif()

            set(_marker "    > [exec] frame ${_frame} (t=${_time}s): ${_command}")
            set(_marker_count 0)
            set(_marker_index -1)
            if(_block_start LESS _block_end)
                math(EXPR _block_last "${_block_end} - 1")
                foreach(_index RANGE ${_block_start} ${_block_last})
                    list(GET _lines ${_index} _line)
                    if(_line STREQUAL _marker)
                        math(EXPR _marker_count "${_marker_count} + 1")
                        set(_marker_index "${_index}")
                    endif()
                endforeach()
            endif()
            if(NOT _marker_count EQUAL 1)
                set(_ok FALSE)
                set(_reason "command '${_command}' marker count was ${_marker_count}, expected 1")
                break()
            endif()

            set(_suffix_lines)
            math(EXPR _suffix_start "${_marker_index} + 1")
            if(_suffix_start LESS _block_end)
                math(EXPR _suffix_last "${_block_end} - 1")
                foreach(_index RANGE ${_suffix_start} ${_suffix_last})
                    list(GET _lines ${_index} _line)
                    list(APPEND _suffix_lines "${_line}")
                endforeach()
            endif()

            set(_semantic_ok TRUE)
            set(_semantic_reason "")
            if(phase STREQUAL "writer" AND (_header_position EQUAL 0 OR _header_position EQUAL 2))
                if(_header_position EQUAL 0)
                    set(_expected_xp 0)
                else()
                    set(_expected_xp 37)
                endif()
                _spark_validate_progression_lines(_suffix_lines "${_expected_xp}" _semantic_ok _semantic_reason)
            elseif(phase STREQUAL "reader" AND (_header_position EQUAL 0 OR _header_position EQUAL 2))
                if(_header_position EQUAL 0)
                    set(_expected_xp 0)
                else()
                    set(_expected_xp 37)
                endif()
                _spark_validate_progression_lines(_suffix_lines "${_expected_xp}" _semantic_ok _semantic_reason)
            elseif(phase STREQUAL "writer" AND _header_position EQUAL 1)
                _spark_count_exact_line(_suffix_lines "    > Awarded 37 XP (level 1)" _result_count)
                if(NOT _result_count EQUAL 1)
                    set(_semantic_ok FALSE)
                    set(_semantic_reason "XP award result count was ${_result_count}, expected 1")
                endif()
            elseif(phase STREQUAL "writer" AND _header_position EQUAL 3)
                _spark_count_exact_line(_suffix_lines
                    "    > Quick save written to slot 'fps_quicksave'" _result_count)
                _spark_count_exact_line(_suffix_lines
                    "    > Quick save FAILED to write slot 'fps_quicksave'" _failure_count)
                if(NOT _result_count EQUAL 1 OR NOT _failure_count EQUAL 0)
                    set(_semantic_ok FALSE)
                    set(_semantic_reason
                        "quicksave success/failure counts were ${_result_count}/${_failure_count}, expected 1/0")
                endif()
            elseif(phase STREQUAL "reader" AND _header_position EQUAL 1)
                _spark_count_exact_line(_suffix_lines
                    "    > Quick load restored level 1 (37 XP)" _result_count)
                _spark_count_exact_line(_suffix_lines
                    "    > Quick load FAILED for slot 'fps_quicksave'" _failure_count)
                if(NOT _result_count EQUAL 1 OR NOT _failure_count EQUAL 0)
                    set(_semantic_ok FALSE)
                    set(_semantic_reason
                        "quickload success/failure counts were ${_result_count}/${_failure_count}, expected 1/0")
                endif()
            endif()

            if(NOT _semantic_ok)
                set(_ok FALSE)
                set(_reason "command '${_command}' evidence failed: ${_semantic_reason}")
                break()
            endif()
        endforeach()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_FPS_SAVE_RELOAD_PARSER_SELF_TEST)
    function(_spark_expect_fps_audit_case name phase result fresh audit expected_ok)
        _spark_validate_fps_audit("${phase}" "${result}" "${fresh}" "${audit}" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "FPS audit parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "FPS audit parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_writer_valid [=[frame 1 t=0.0s | ok  | level
    > [exec] frame 1 (t=0.0s): level
    > === Progression ===
Level: 1/50
XP: 0/282
Progress: 0%
frame 2 t=0.0s | ok  | xp 37
    > [exec] frame 2 (t=0.0s): xp 37
    > Awarded 37 XP (level 1)
frame 3 t=0.1s | ok  | level
    > [exec] frame 3 (t=0.1s): level
    > === Progression ===
Level: 1/50
XP: 37/282
Progress: 13%
frame 4 t=0.1s | ok  | quicksave
    > [exec] frame 4 (t=0.1s): quicksave
    > Quick save written to slot 'fps_quicksave'
]=])
    set(_reader_valid [=[frame 1 t=0.0s | ok  | level
    > [exec] frame 1 (t=0.0s): level
    > === Progression ===
Level: 1/50
XP: 0/282
frame 2 t=0.0s | ok  | quickload
    > [exec] frame 2 (t=0.0s): quickload
    > Quick load restored level 1 (37 XP)
frame 3 t=0.1s | ok  | level
    > [exec] frame 3 (t=0.1s): level
    > === Progression ===
Level: 1/50
XP: 37/282
]=])

    _spark_expect_fps_audit_case(writer-valid writer 0 TRUE "${_writer_valid}" TRUE)
    _spark_expect_fps_audit_case(reader-valid reader 0 TRUE "${_reader_valid}" TRUE)
    string(REPLACE "\n" "\r\n" _writer_crlf "${_writer_valid}")
    _spark_expect_fps_audit_case(writer-crlf writer 0 TRUE "${_writer_crlf}" TRUE)
    _spark_expect_fps_audit_case(not-fresh writer 0 FALSE "${_writer_valid}" FALSE)
    _spark_expect_fps_audit_case(nonzero writer 2 TRUE "${_writer_valid}" FALSE)
    _spark_expect_fps_audit_case(duplicate writer 0 TRUE "${_writer_valid}${_writer_valid}" FALSE)
    set(_malformed "frame malformed\n${_writer_valid}")
    _spark_expect_fps_audit_case(malformed-header writer 0 TRUE "${_malformed}" FALSE)
    string(REPLACE "xp 37" "xp 38" _wrong_command "${_writer_valid}")
    _spark_expect_fps_audit_case(wrong-command writer 0 TRUE "${_wrong_command}" FALSE)
    string(REPLACE "    > Quick save written to slot 'fps_quicksave'" ""
        _dispatch_only "${_writer_valid}")
    _spark_expect_fps_audit_case(dispatch-only writer 0 TRUE "${_dispatch_only}" FALSE)
    string(REPLACE "Quick save written to slot 'fps_quicksave'"
        "Quick save FAILED to write slot 'fps_quicksave'" _save_failure "${_writer_valid}")
    _spark_expect_fps_audit_case(save-failure writer 0 TRUE "${_save_failure}" FALSE)
    string(REPLACE
        "    > [exec] frame 4 (t=0.1s): quicksave\n    > Quick save written to slot 'fps_quicksave'"
        "    > Quick save written to slot 'fps_quicksave'\n    > [exec] frame 4 (t=0.1s): quicksave"
        _stale_success "${_writer_valid}")
    _spark_expect_fps_audit_case(stale-success-before-marker writer 0 TRUE "${_stale_success}" FALSE)
    string(REPLACE "XP: 37/282" "XP: 38/282" _wrong_xp "${_writer_valid}")
    _spark_expect_fps_audit_case(wrong-xp writer 0 TRUE "${_wrong_xp}" FALSE)
    string(REPLACE "    > [exec] frame 2 (t=0.0s): quickload" ""
        _missing_marker "${_reader_valid}")
    _spark_expect_fps_audit_case(missing-marker reader 0 TRUE "${_missing_marker}" FALSE)
    string(REPLACE "Quick load restored level 1 (37 XP)"
        "Quick load FAILED for slot 'fps_quicksave'" _load_failure "${_reader_valid}")
    _spark_expect_fps_audit_case(load-failure reader 0 TRUE "${_load_failure}" FALSE)
    message(STATUS "Installed FPS save/reload audit parser contract passed")
    return()
endif()

foreach(_required IN ITEMS
        SPARK_INSTALLED_ROOT
        SPARK_FPS_SAVE_TEST_ROOT
        SPARK_SOURCE_SHA
        SPARK_SOURCE_TREE_STATE
        SPARK_BUILD_CONFIG)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunInstalledFPSSaveReload.cmake requires -D${_required}=<value>")
    endif()
endforeach()
foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_FPS_SAVE_TEST_ROOT)
    if(NOT IS_ABSOLUTE "${${_required}}" OR "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()
if(NOT SPARK_FPS_PACKAGE_PREVALIDATED)
    message(FATAL_ERROR
        "RunInstalledFPSSaveReload.cmake requires the trusted package validator to run first")
endif()
string(LENGTH "${SPARK_SOURCE_SHA}" _source_sha_length)
if(NOT _source_sha_length EQUAL 40 OR SPARK_SOURCE_SHA MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "SPARK_SOURCE_SHA must be a canonical 40-character commit")
endif()
if(NOT SPARK_SOURCE_TREE_STATE MATCHES "^(clean|dirty)$")
    message(FATAL_ERROR "SPARK_SOURCE_TREE_STATE must be clean or dirty")
endif()
if(NOT SPARK_BUILD_CONFIG MATCHES "^[A-Za-z0-9_.+-]+$")
    message(FATAL_ERROR "SPARK_BUILD_CONFIG has an invalid token")
endif()

set(_bin "${SPARK_INSTALLED_ROOT}/bin")
set(_engine "${_bin}/SparkEngine.exe")
set(_module "${_bin}/SparkGameFPS.dll")
foreach(_required_file IN ITEMS "${_engine}" "${_module}")
    if(NOT EXISTS "${_required_file}" OR IS_DIRECTORY "${_required_file}" OR IS_SYMLINK "${_required_file}")
        message(FATAL_ERROR "Installed FPS save/reload input is missing or unsafe: ${_required_file}")
    endif()
endforeach()
file(SHA256 "${_engine}" _engine_sha256)
file(SHA256 "${_module}" _module_sha256)
file(SHA256 "${_runner_script}" _runner_sha256)
cmake_host_system_information(RESULT _host_os_name QUERY OS_NAME)
cmake_host_system_information(RESULT _host_os_release QUERY OS_RELEASE)
cmake_host_system_information(RESULT _host_os_version QUERY OS_VERSION)
cmake_host_system_information(RESULT _host_os_platform QUERY OS_PLATFORM)
if(IS_DIRECTORY "${_bin}/Saves" OR EXISTS "${_bin}/Saves")
    message(FATAL_ERROR "Installed FPS save/reload refuses a staged legacy Saves directory")
endif()

if(EXISTS "${SPARK_FPS_SAVE_TEST_ROOT}" OR IS_SYMLINK "${SPARK_FPS_SAVE_TEST_ROOT}")
    message(FATAL_ERROR
        "Installed FPS save/reload test root must be absent before the run: "
        "${SPARK_FPS_SAVE_TEST_ROOT}")
endif()
file(MAKE_DIRECTORY "${SPARK_FPS_SAVE_TEST_ROOT}")
set(_local_app_data "${SPARK_FPS_SAVE_TEST_ROOT}/localappdata")
file(MAKE_DIRECTORY "${_local_app_data}")
set(_audit "${_bin}/exec_audit.log")
file(LOCK "${_bin}/.fps-save-reload.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE _lock_result)
if(NOT "${_lock_result}" STREQUAL "0")
    message(FATAL_ERROR "Installed FPS save/reload audit namespace is already in use: ${_lock_result}")
endif()
set(_save "${_local_app_data}/SparkEngine/Saves/fps_quicksave.spark_save")
set(_backup "${_save}.bak")
if(EXISTS "${_save}" OR EXISTS "${_backup}")
    message(FATAL_ERROR "Installed FPS save/reload test root is not fresh")
endif()

set(_writer_script "${SPARK_FPS_SAVE_TEST_ROOT}/writer-exec.txt")
set(_reader_script "${SPARK_FPS_SAVE_TEST_ROOT}/reader-exec.txt")
file(WRITE "${_writer_script}" "1 level\n2 xp 37\n3 level\n4 quicksave\n")
file(WRITE "${_reader_script}" "1 level\n2 quickload\n3 level\n")

function(_spark_run_fps_phase phase script)
    file(REMOVE "${_audit}")
    if(EXISTS "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Could not establish a fresh exec audit before ${phase}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "LOCALAPPDATA=${_local_app_data}"
            "SPARK_RHI_BACKEND=d3d11"
            "SPARK_D3D11_DRIVER=warp"
            "${_engine}"
            -game "${_module}"
            -require-game
            -exec "${script}"
            -test-frames 30
            -threads 2
            -window-size 640x360
            -no-subprocess
        WORKING_DIRECTORY "${_bin}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT 90
        ENCODING UTF-8)
    file(WRITE "${SPARK_FPS_SAVE_TEST_ROOT}/${phase}-stdout.log" "${_stdout}")
    file(WRITE "${SPARK_FPS_SAVE_TEST_ROOT}/${phase}-stderr.log" "${_stderr}")

    _spark_validate_lifecycle_result("${_result}" "${_stdout}" "${_stderr}"
        _lifecycle_ok _lifecycle_reason)
    if(NOT _lifecycle_ok)
        message(FATAL_ERROR
            "Installed FPS ${phase} lifecycle failed: ${_lifecycle_reason}\n"
            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()

    if(NOT EXISTS "${_audit}" OR IS_DIRECTORY "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Installed FPS ${phase} did not produce a regular fresh exec audit")
    endif()
    file(SIZE "${_audit}" _audit_size)
    if(_audit_size LESS 1 OR _audit_size GREATER 1048576)
        message(FATAL_ERROR "Installed FPS ${phase} exec audit size ${_audit_size} is invalid")
    endif()
    file(READ "${_audit}" _audit_content)
    set(_audit_archive "${SPARK_FPS_SAVE_TEST_ROOT}/${phase}-exec-audit.log")
    file(COPY_FILE "${_audit}" "${_audit_archive}" ONLY_IF_DIFFERENT)
    _spark_validate_fps_audit("${phase}" "${_result}" TRUE "${_audit_content}"
        _audit_ok _audit_reason)
    if(NOT _audit_ok)
        message(FATAL_ERROR
            "Installed FPS ${phase} exec audit failed: ${_audit_reason}\n"
            "audit retained at ${_audit_archive}\n"
            "audit:\n${_audit_content}")
    endif()
    file(REMOVE "${_audit}")
endfunction()

_spark_run_fps_phase(writer "${_writer_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Installed FPS writer did not create a regular quicksave: ${_save}")
endif()
file(SIZE "${_save}" _save_size)
if(_save_size LESS 1 OR _save_size GREATER 67108864)
    message(FATAL_ERROR "Installed FPS quicksave size ${_save_size} is invalid")
endif()
file(SHA256 "${_save}" _writer_save_sha256)

_spark_run_fps_phase(reader "${_reader_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Installed FPS reader lost the quicksave: ${_save}")
endif()
file(SHA256 "${_save}" _reader_save_sha256)
if(NOT _writer_save_sha256 STREQUAL _reader_save_sha256)
    message(FATAL_ERROR
        "Installed FPS reader mutated the persisted quicksave:\n"
        "  before: ${_writer_save_sha256}\n  after:  ${_reader_save_sha256}")
endif()

file(WRITE "${SPARK_FPS_SAVE_TEST_ROOT}/evidence.txt"
    "source_sha=${SPARK_SOURCE_SHA}\n"
    "source_tree_state=${SPARK_SOURCE_TREE_STATE}\n"
    "build_config=${SPARK_BUILD_CONFIG}\n"
    "host_system=${_host_os_name}\n"
    "host_release=${_host_os_release}\n"
    "host_version=${_host_os_version}\n"
    "host_processor=${_host_os_platform}\n"
    "engine_sha256=${_engine_sha256}\n"
    "module_sha256=${_module_sha256}\n"
    "runner_sha256=${_runner_sha256}\n"
    "save_sha256=${_writer_save_sha256}\n"
    "save_bytes=${_save_size}\n"
    "writer_xp=37\nreader_restored_xp=37\n"
    "backend=d3d11-warp\n")
message(STATUS
    "Installed FPS save/reload persistence passed across two fresh WARP processes "
    "(save ${_writer_save_sha256})")
