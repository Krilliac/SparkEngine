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

# SPARK_FPS_DEVELOPER_COMMANDS says whether the build under test registers the FPS developer
# commands (Console/FPSConsolePolicy.h): ON for every build but plain Shipping, OFF for
# MinSizeRel without ENABLE_DEVCOMMANDS_IN_SHIPPING. Undefined means ON. The result is
# TRUE or FALSE; any other spelling fails the run rather than silently picking a profile.
function(_spark_fps_developer_commands_enabled out_var)
    if(NOT DEFINED SPARK_FPS_DEVELOPER_COMMANDS)
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    string(TOUPPER "${SPARK_FPS_DEVELOPER_COMMANDS}" _value)
    if(_value MATCHES "^(ON|TRUE|1)$")
        set(${out_var} TRUE PARENT_SCOPE)
    elseif(_value MATCHES "^(OFF|FALSE|0)$")
        set(${out_var} FALSE PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "SPARK_FPS_DEVELOPER_COMMANDS must be ON, OFF, TRUE, FALSE, 1 or 0, got '${SPARK_FPS_DEVELOPER_COMMANDS}'")
    endif()
endfunction()

# Counts the lines of lines_var that start with prefix.
function(_spark_count_prefix_lines lines_var prefix out_count)
    set(_count 0)
    foreach(_line IN LISTS ${lines_var})
        string(FIND "${_line}" "${prefix}" _position)
        if(_position EQUAL 0)
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    set(${out_count} "${_count}" PARENT_SCOPE)
endfunction()

# Plain Shipping drops the developer commands, so the writer's `xp 37` must be refused: its
# header is ERR, the console answers "Unknown command: 'xp'." and nothing is awarded. An
# unregistered command with arguments is shown redacted in the audit (fail closed), hence
# the header and marker name `xp <arguments-redacted>`. Value persistence of non-zero XP is
# proven by the Release/Debug lanes, which run with developer commands.
function(_spark_validate_fps_audit phase child_result audit_fresh audit out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    _spark_fps_developer_commands_enabled(_dev_commands)
    if(_dev_commands)
        set(_saved_xp 37)
    else()
        set(_saved_xp 0)
    endif()

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
            if(_dev_commands)
                set(_expected_commands "level" "xp 37" "level" "quicksave")
            else()
                set(_expected_commands "level" "xp <arguments-redacted>" "level" "quicksave")
            endif()
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
            set(_expected_status "ok ")
            if(NOT _dev_commands AND phase STREQUAL "writer" AND _header_position EQUAL 1)
                set(_expected_status "ERR")
            endif()
            if(NOT _status STREQUAL _expected_status)
                set(_ok FALSE)
                set(_reason "command '${_command}' had dispatch header '${_status}', expected '${_expected_status}'")
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

            set(_marker "    > [exec] frame ${_frame} (t=${_time}s, entry=${_header_position}): ${_command}")
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
                    set(_expected_xp ${_saved_xp})
                endif()
                _spark_validate_progression_lines(_suffix_lines "${_expected_xp}" _semantic_ok _semantic_reason)
            elseif(phase STREQUAL "reader" AND (_header_position EQUAL 0 OR _header_position EQUAL 2))
                if(_header_position EQUAL 0)
                    set(_expected_xp 0)
                else()
                    set(_expected_xp ${_saved_xp})
                endif()
                _spark_validate_progression_lines(_suffix_lines "${_expected_xp}" _semantic_ok _semantic_reason)
            elseif(phase STREQUAL "writer" AND _header_position EQUAL 1 AND _dev_commands)
                _spark_count_exact_line(_suffix_lines "    > Awarded 37 XP (level 1)" _result_count)
                if(NOT _result_count EQUAL 1)
                    set(_semantic_ok FALSE)
                    set(_semantic_reason "XP award result count was ${_result_count}, expected 1")
                endif()
            elseif(phase STREQUAL "writer" AND _header_position EQUAL 1)
                _spark_count_prefix_lines(_suffix_lines "    > Unknown command: 'xp'." _unknown_count)
                _spark_count_prefix_lines(_suffix_lines "    > Awarded" _awarded_count)
                if(NOT _unknown_count EQUAL 1 OR NOT _awarded_count EQUAL 0)
                    set(_semantic_ok FALSE)
                    set(_semantic_reason
                        "developer-command gate leaked: unknown-command/award line counts were "
                        "${_unknown_count}/${_awarded_count}, expected 1/0")
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
                    "    > Quick load restored level 1 (${_saved_xp} XP)" _result_count)
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

# Lets the parser self-test prove that an invalid profile spelling is a hard error.
if(SPARK_FPS_PROFILE_PROBE)
    _spark_fps_developer_commands_enabled(_probe_profile)
    return()
endif()

# cmake/RunSparkHeadlessFPSSaveReload.cmake reuses the exact writer/reader audit
# validator above for the source-tree NullRHI save/reload run.
if(SPARK_FPS_SAVE_RELOAD_PARSER_ONLY)
    return()
endif()

if(SPARK_FPS_SAVE_RELOAD_PARSER_SELF_TEST)
    # The optional seventh argument is the SPARK_FPS_DEVELOPER_COMMANDS profile; the default is ON.
    function(_spark_expect_fps_audit_case name phase result fresh audit expected_ok)
        if(ARGC GREATER 6)
            set(SPARK_FPS_DEVELOPER_COMMANDS "${ARGV6}")
        else()
            set(SPARK_FPS_DEVELOPER_COMMANDS ON)
        endif()
        _spark_validate_fps_audit("${phase}" "${result}" "${fresh}" "${audit}" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "FPS audit parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "FPS audit parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_writer_valid [=[frame 1 t=0.0s | ok  | level
    > [exec] frame 1 (t=0.0s, entry=0): level
    > === Progression ===
Level: 1/50
XP: 0/282
Progress: 0%
frame 2 t=0.0s | ok  | xp 37
    > [exec] frame 2 (t=0.0s, entry=1): xp 37
    > Awarded 37 XP (level 1)
frame 3 t=0.1s | ok  | level
    > [exec] frame 3 (t=0.1s, entry=2): level
    > === Progression ===
Level: 1/50
XP: 37/282
Progress: 13%
frame 4 t=0.1s | ok  | quicksave
    > [exec] frame 4 (t=0.1s, entry=3): quicksave
    > Quick save written to slot 'fps_quicksave'
]=])
    set(_reader_valid [=[frame 1 t=0.0s | ok  | level
    > [exec] frame 1 (t=0.0s, entry=0): level
    > === Progression ===
Level: 1/50
XP: 0/282
frame 2 t=0.0s | ok  | quickload
    > [exec] frame 2 (t=0.0s, entry=1): quickload
    > Quick load restored level 1 (37 XP)
frame 3 t=0.1s | ok  | level
    > [exec] frame 3 (t=0.1s, entry=2): level
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
        "    > [exec] frame 4 (t=0.1s, entry=3): quicksave\n    > Quick save written to slot 'fps_quicksave'"
        "    > Quick save written to slot 'fps_quicksave'\n    > [exec] frame 4 (t=0.1s, entry=3): quicksave"
        _stale_success "${_writer_valid}")
    _spark_expect_fps_audit_case(stale-success-before-marker writer 0 TRUE "${_stale_success}" FALSE)
    string(REPLACE "XP: 37/282" "XP: 38/282" _wrong_xp "${_writer_valid}")
    _spark_expect_fps_audit_case(wrong-xp writer 0 TRUE "${_wrong_xp}" FALSE)
    string(REPLACE "    > [exec] frame 2 (t=0.0s, entry=1): quickload" ""
        _missing_marker "${_reader_valid}")
    _spark_expect_fps_audit_case(missing-marker reader 0 TRUE "${_missing_marker}" FALSE)
    string(REPLACE "Quick load restored level 1 (37 XP)"
        "Quick load FAILED for slot 'fps_quicksave'" _load_failure "${_reader_valid}")
    _spark_expect_fps_audit_case(load-failure reader 0 TRUE "${_load_failure}" FALSE)

    # Plain Shipping profile (developer commands OFF): the writer's xp is refused, the saved
    # state is level 1 with 0 XP, and the same audits must fail the developer profile.
    set(_xp_ok_block "frame 2 t=0.0s | ok  | xp 37
    > [exec] frame 2 (t=0.0s, entry=1): xp 37
    > Awarded 37 XP (level 1)
")
    set(_xp_refused_block "frame 2 t=0.0s | ERR | xp <arguments-redacted>
    > [exec] frame 2 (t=0.0s, entry=1): xp <arguments-redacted>
    > Unknown command: 'xp'. Type 'help' for available commands.
")
    string(REPLACE "${_xp_ok_block}" "${_xp_refused_block}" _writer_shipping "${_writer_valid}")
    string(REPLACE "XP: 37/282" "XP: 0/282" _writer_shipping "${_writer_shipping}")
    string(REPLACE "Quick load restored level 1 (37 XP)" "Quick load restored level 1 (0 XP)"
        _reader_shipping "${_reader_valid}")
    string(REPLACE "XP: 37/282" "XP: 0/282" _reader_shipping "${_reader_shipping}")
    if(_writer_shipping STREQUAL _writer_valid OR _reader_shipping STREQUAL _reader_valid)
        message(FATAL_ERROR "FPS audit parser self-test could not derive the Shipping fixtures")
    endif()

    _spark_expect_fps_audit_case(shipping-writer-valid writer 0 TRUE "${_writer_shipping}" TRUE OFF)
    _spark_expect_fps_audit_case(shipping-reader-valid reader 0 TRUE "${_reader_shipping}" TRUE OFF)
    _spark_expect_fps_audit_case(shipping-profile-spelled-false writer 0 TRUE "${_writer_shipping}" TRUE false)
    _spark_expect_fps_audit_case(shipping-profile-spelled-zero reader 0 TRUE "${_reader_shipping}" TRUE 0)
    string(REPLACE "\n" "\r\n" _writer_shipping_crlf "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-writer-crlf writer 0 TRUE "${_writer_shipping_crlf}" TRUE OFF)
    _spark_expect_fps_audit_case(shipping-writer-not-fresh writer 0 FALSE "${_writer_shipping}" FALSE OFF)

    # Gate leak: a developer audit (xp registered and awarded) must fail the Shipping profile.
    _spark_expect_fps_audit_case(gate-leak-writer writer 0 TRUE "${_writer_valid}" FALSE OFF)
    _spark_expect_fps_audit_case(gate-leak-reader reader 0 TRUE "${_reader_valid}" FALSE OFF)
    string(REPLACE "xp <arguments-redacted>" "xp 37" _shipping_unredacted "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-unredacted-command writer 0 TRUE "${_shipping_unredacted}" FALSE OFF)
    string(REPLACE "| ERR | xp" "| ok  | xp" _shipping_ok_header "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-ok-header writer 0 TRUE "${_shipping_ok_header}" FALSE OFF)
    string(REPLACE "    > Unknown command: 'xp'. Type 'help' for available commands.\n"
        "    > Unknown command: 'xp'. Type 'help' for available commands.\n    > Awarded 37 XP (level 1)\n"
        _shipping_award "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-awarded writer 0 TRUE "${_shipping_award}" FALSE OFF)
    string(REPLACE "    > Unknown command: 'xp'. Type 'help' for available commands.\n" ""
        _shipping_no_unknown "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-no-unknown-line writer 0 TRUE "${_shipping_no_unknown}" FALSE OFF)
    string(REPLACE "    > Unknown command: 'xp'. Type 'help' for available commands.\n"
        "    > Unknown command: 'xp'. Type 'help' for available commands.\n    > Unknown command: 'xp'.\n"
        _shipping_two_unknown "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-two-unknown-lines writer 0 TRUE "${_shipping_two_unknown}" FALSE OFF)
    string(REPLACE "    > [exec] frame 2 (t=0.0s, entry=1): xp <arguments-redacted>\n" ""
        _shipping_no_marker "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-missing-marker writer 0 TRUE "${_shipping_no_marker}" FALSE OFF)
    string(REPLACE "Unknown command: 'xp'." "Unknown command: 'god'." _shipping_other_unknown "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-xp-wrong-unknown-name writer 0 TRUE
        "${_shipping_other_unknown}" FALSE OFF)
    string(REPLACE "XP: 0/282\nProgress: 13%" "XP: 37/282\nProgress: 13%" _shipping_xp_grew "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-writer-xp-grew writer 0 TRUE "${_shipping_xp_grew}" FALSE OFF)
    string(REPLACE "Quick load restored level 1 (0 XP)" "Quick load restored level 1 (37 XP)"
        _shipping_restored_37 "${_reader_shipping}")
    _spark_expect_fps_audit_case(shipping-reader-restored-37 reader 0 TRUE "${_shipping_restored_37}" FALSE OFF)
    string(REPLACE "Quick load restored level 1 (0 XP)" "Quick load FAILED for slot 'fps_quicksave'"
        _shipping_load_failure "${_reader_shipping}")
    _spark_expect_fps_audit_case(shipping-reader-load-failure reader 0 TRUE "${_shipping_load_failure}" FALSE OFF)
    string(REPLACE "    > Quick save written to slot 'fps_quicksave'" "" _shipping_dispatch_only "${_writer_shipping}")
    _spark_expect_fps_audit_case(shipping-dispatch-only writer 0 TRUE "${_shipping_dispatch_only}" FALSE OFF)

    # The developer profile rejects a Shipping audit: nothing was awarded, so nothing proves xp.
    _spark_expect_fps_audit_case(developer-rejects-shipping-writer writer 0 TRUE "${_writer_shipping}" FALSE ON)
    _spark_expect_fps_audit_case(developer-rejects-shipping-reader reader 0 TRUE "${_reader_shipping}" FALSE ON)
    _spark_expect_fps_audit_case(developer-profile-spelled-true writer 0 TRUE "${_writer_valid}" TRUE TRUE)
    _spark_expect_fps_audit_case(developer-profile-spelled-one reader 0 TRUE "${_reader_valid}" TRUE 1)

    # Any other profile spelling is a hard error, not a silent default.
    foreach(_bad_profile IN ITEMS "" "maybe" "2" "ONN")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DSPARK_SOURCE_ROOT=${SPARK_SOURCE_ROOT}"
                "-DSPARK_FPS_SAVE_RELOAD_PARSER_ONLY=ON" "-DSPARK_FPS_DEVELOPER_COMMANDS=${_bad_profile}"
                "-DSPARK_FPS_PROFILE_PROBE=ON" -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE _bad_profile_result
            OUTPUT_QUIET ERROR_QUIET)
        if(_bad_profile_result EQUAL 0)
            message(FATAL_ERROR "FPS audit parser accepted the invalid developer-command profile '${_bad_profile}'")
        endif()
    endforeach()
    message(STATUS "Installed FPS save/reload audit parser contract passed (developer-commands ON and OFF profiles)")
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
_spark_fps_developer_commands_enabled(_dev_commands)
if(_dev_commands)
    set(_profile_name ON)
    set(_saved_xp 37)
else()
    set(_profile_name OFF)
    set(_saved_xp 0)
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
    "developer_commands=${_profile_name}\n"
    "writer_xp=${_saved_xp}\nreader_restored_xp=${_saved_xp}\n"
    "backend=d3d11-warp\n")
message(STATUS
    "Installed FPS save/reload persistence passed across two fresh WARP processes "
    "(save ${_writer_save_sha256}; developer-commands ${_profile_name} profile validated)")
