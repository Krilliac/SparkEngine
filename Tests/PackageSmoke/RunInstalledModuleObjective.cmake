cmake_minimum_required(VERSION 3.25)

# MOD-330/340/350/370/380: install the configured engine plus one experimental
# game module (runtime + samples components), then drive the INSTALLED
# SparkEngine and module image headless on NullRHI through a per-module
# objective spec (Tests/PackageSmoke/ModuleObjectives/<Module>.cmake).
#
# Every phase is a fresh process. All phases of one run share one isolated user
# root, so a save written by one phase is what the next phase loads.
#
# The spec sets SPARK_OBJECTIVE_PHASES and, per phase P:
#   SPARK_OBJECTIVE_<P>_SCRIPT   -exec text: "<frame> cmd" or "t<seconds> cmd" lines (Core/ExecScript.h)
#   SPARK_OBJECTIVE_<P>_SECONDS  the -test-seconds limit
#   SPARK_OBJECTIVE_<P>_EXPECT   "selector|regex" rules; the regex must match the selected block's output
#   SPARK_OBJECTIVE_<P>_REJECT   "selector|regex" rules; the regex must NOT match the selected block's output
#   SPARK_OBJECTIVE_<P>_SAME_AS  "selector|phase" or "selector|phase|selector" rules: the output must be
#                                byte-identical to that block of an EARLIER phase (another process)
# Script lines must already be in due-time order: the audit headers are checked
# against the scripted commands in file order.
# A selector is a command exactly as written in the script. It names that
# command's LAST block, or its Nth block as "command#N" (1-based). A block's
# output is only what the audit recorded AFTER that command's own
# "[exec] frame N (t=X.Xs, entry=I): command" marker (I is the zero-based
# index in the due-ordered schedule), so output left in the console
# window by an earlier command can never satisfy a rule.
# A spec may also define `spark_objective_verify_<P>(phase out_ok out_reason)`
# for rules the generic forms cannot express. It reads the blocks through
# spark_objective_block_order() (every "command#N" in audit order),
# spark_objective_block_count() and spark_objective_block_output().
#
# A phase passes only when the process passes the shared NullRHI lifecycle
# parser (one module, faults=0, no D3D11 device), every scripted command has an
# "ok" audit header with exactly one marker (the -test-seconds limit did not
# cut the script short), every rule holds, and nothing it printed names the
# source or build tree outside this run's own root (the installed payload must
# not reach back into either). On POSIX hosts each process starts from an empty
# environment (`env -i`) plus the isolated user root. stdout, stderr and the
# audit of each phase stay under the run root.
#
# This is local opt-in evidence of an experimental module. It deliberately
# writes no package-smoke-v1 record: that format is fixed to the stable-v1 FPS
# profile (tools/module-evidence/artifacts.py), which RDY-020 keeps these
# modules out of.

set(_runner_script "${CMAKE_CURRENT_LIST_FILE}")
get_filename_component(_runner_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${_runner_source_root}/cmake/RunSparkHeadlessNullRHILifecycle.cmake")
unset(SPARK_HEADLESS_NULLRHI_PARSER_ONLY)

set(_spark_objective_header_regex "^frame ([0-9]+) t=([0-9]+\\.[0-9])s \\| (ok |ERR) \\| (.+)$")

# The scheduled entries of an -exec script the way ParseExecScript reads them:
# every non-blank line that does not start with '#', as "<frame> command",
# "t<seconds> command" or a bare command (frame 0). Due times are reported in
# milliseconds, frame entries at the nominal 60 fps ExecScript orders them by.
function(_spark_objective_script_entries script out_commands out_due_ms)
    string(REPLACE "\r\n" "\n" _text "${script}")
    string(REPLACE ";" "\\;" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")
    set(_commands)
    set(_due)
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        set(_ms 0)
        if(_line MATCHES "^t([0-9]+)(\\.([0-9]+))?[ \t]+(.+)$")
            set(_whole "${CMAKE_MATCH_1}")
            set(_fraction "${CMAKE_MATCH_3}000")
            set(_line "${CMAKE_MATCH_4}")
            string(SUBSTRING "${_fraction}" 0 3 _fraction)
            string(REGEX REPLACE "^0+([0-9])" "\\1" _fraction "${_fraction}")
            math(EXPR _ms "${_whole} * 1000 + ${_fraction}")
        elseif(_line MATCHES "^([0-9]+)[ \t]+(.+)$")
            math(EXPR _ms "${CMAKE_MATCH_1} * 1000 / 60")
            set(_line "${CMAKE_MATCH_2}")
        endif()
        list(APPEND _commands "${_line}")
        list(APPEND _due "${_ms}")
    endforeach()
    set(${out_commands} "${_commands}" PARENT_SCOPE)
    set(${out_due_ms} "${_due}" PARENT_SCOPE)
endfunction()

# Whether "command" or "command#N" names a block the scripted commands will produce.
function(_spark_objective_selector_scripted selector commands out_ok)
    set(_command "${selector}")
    set(_index 1)
    if(selector MATCHES "^(.+)#([1-9][0-9]*)$")
        set(_command "${CMAKE_MATCH_1}")
        set(_index "${CMAKE_MATCH_2}")
    endif()
    set(_count 0)
    foreach(_scripted IN LISTS commands)
        if(_scripted STREQUAL _command)
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(_count GREATER 0 AND NOT _index GREATER _count)
        set(${out_ok} TRUE PARENT_SCOPE)
    else()
        set(${out_ok} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Static checks of the included spec before any process runs: named phases with
# whole -test-seconds, scripts in due-time order that finish before the limit,
# and rules whose selectors name scripted commands of this or an earlier phase.
function(_spark_objective_lint_spec out_ok out_reason)
    set(${out_ok} FALSE PARENT_SCOPE)
    if(NOT SPARK_OBJECTIVE_PHASES)
        set(${out_reason} "no SPARK_OBJECTIVE_PHASES" PARENT_SCOPE)
        return()
    endif()
    set(_linted)
    foreach(_phase IN LISTS SPARK_OBJECTIVE_PHASES)
        if(NOT _phase MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR _phase IN_LIST _linted OR
           NOT "${SPARK_OBJECTIVE_${_phase}_SECONDS}" MATCHES "^[1-9][0-9]*$")
            set(${out_reason} "phase '${_phase}' needs a unique name and whole -test-seconds" PARENT_SCOPE)
            return()
        endif()
        _spark_objective_script_entries("${SPARK_OBJECTIVE_${_phase}_SCRIPT}" _commands _due)
        set(_commands_${_phase} "${_commands}")
        if(NOT _commands)
            set(${out_reason} "phase '${_phase}' has no scripted commands" PARENT_SCOPE)
            return()
        endif()
        set(_previous 0)
        foreach(_ms IN LISTS _due)
            if(_ms LESS _previous)
                set(${out_reason} "phase '${_phase}' script is not in due-time order" PARENT_SCOPE)
                return()
            endif()
            set(_previous "${_ms}")
        endforeach()
        math(EXPR _limit_ms "${SPARK_OBJECTIVE_${_phase}_SECONDS} * 1000")
        if(NOT _previous LESS _limit_ms)
            set(${out_reason} "phase '${_phase}' schedules a command at or after its -test-seconds limit" PARENT_SCOPE)
            return()
        endif()
        foreach(_kind IN ITEMS EXPECT REJECT)
            foreach(_rule IN LISTS SPARK_OBJECTIVE_${_phase}_${_kind})
                if(NOT _rule MATCHES "^([^|]+)\\|(.+)$")
                    set(${out_reason} "phase '${_phase}' has a malformed ${_kind} rule '${_rule}'" PARENT_SCOPE)
                    return()
                endif()
                _spark_objective_selector_scripted("${CMAKE_MATCH_1}" "${_commands}" _scripted)
                if(NOT _scripted)
                    set(${out_reason} "phase '${_phase}' ${_kind} rule '${_rule}' names no scripted block" PARENT_SCOPE)
                    return()
                endif()
            endforeach()
        endforeach()
        foreach(_rule IN LISTS SPARK_OBJECTIVE_${_phase}_SAME_AS)
            string(REPLACE "|" ";" _fields "${_rule}")
            list(LENGTH _fields _field_count)
            if(_field_count LESS 2 OR _field_count GREATER 3)
                set(${out_reason} "phase '${_phase}' has a malformed SAME_AS rule '${_rule}'" PARENT_SCOPE)
                return()
            endif()
            list(GET _fields 0 _selector)
            list(GET _fields 1 _other_phase)
            set(_other_selector "${_selector}")
            if(_field_count EQUAL 3)
                list(GET _fields 2 _other_selector)
            endif()
            if(NOT _other_phase IN_LIST _linted)
                set(${out_reason} "phase '${_phase}' SAME_AS rule '${_rule}' names no earlier phase" PARENT_SCOPE)
                return()
            endif()
            _spark_objective_selector_scripted("${_selector}" "${_commands}" _own_scripted)
            _spark_objective_selector_scripted("${_other_selector}" "${_commands_${_other_phase}}" _other_scripted)
            if(NOT _own_scripted OR NOT _other_scripted)
                set(${out_reason} "phase '${_phase}' SAME_AS rule '${_rule}' names no scripted block" PARENT_SCOPE)
                return()
            endif()
        endforeach()
        list(APPEND _linted "${_phase}")
    endforeach()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

# Blocks are stored as global properties so rules of a later phase can compare
# against an earlier phase's output.
function(spark_objective_block_count phase command out_count)
    get_property(_count GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_COUNT:${command}")
    if(NOT _count)
        set(_count 0)
    endif()
    set(${out_count} "${_count}" PARENT_SCOPE)
endfunction()

function(spark_objective_block_output phase command index out_output)
    get_property(_output GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_OUT:${command}#${index}")
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

function(spark_objective_block_order phase out_list)
    get_property(_order GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_ORDER")
    set(${out_list} "${_order}" PARENT_SCOPE)
endfunction()

# Resolve "command" (last block) or "command#N" into its output.
function(_spark_objective_select phase selector out_found out_output)
    set(_command "${selector}")
    set(_index "")
    if(selector MATCHES "^(.+)#([1-9][0-9]*)$")
        set(_command "${CMAKE_MATCH_1}")
        set(_index "${CMAKE_MATCH_2}")
    endif()
    spark_objective_block_count("${phase}" "${_command}" _count)
    if(_index STREQUAL "")
        set(_index "${_count}")
    endif()
    if(_count EQUAL 0 OR _index GREATER _count)
        set(${out_found} FALSE PARENT_SCOPE)
        set(${out_output} "" PARENT_SCOPE)
        return()
    endif()
    spark_objective_block_output("${phase}" "${_command}" "${_index}" _output)
    set(${out_found} TRUE PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

# Split the audit into blocks in one linear pass and store each command's
# post-marker output. Fails on a malformed or ERR header, a block without
# exactly one marker for its schedule index, or a header sequence that differs
# from the script. Prior markers may recur before the current marker because
# AppendAudit copies a console history window, even within the same frame.
function(_spark_objective_parse_audit phase audit expected_commands out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    string(REPLACE "\r\n" "\n" _text "${audit}")
    string(REPLACE "\r" "\n" _text "${_text}")
    # The audit ends with a newline; without this the file's last block would gain an empty line.
    string(REGEX REPLACE "\n$" "" _text "${_text}")
    string(REPLACE ";" "\\;" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")

    set(_seen_commands)
    set(_seen_markers)
    set(_in_block FALSE)
    set(_command "")
    set(_marker "")
    set(_markers 0)
    set(_output "")
    set(_have_output FALSE)

    macro(_spark_objective_close_block)
        if(_in_block)
            if(NOT _markers EQUAL 1)
                set(_ok FALSE)
                set(_reason "command '${_command}' entry ${_entry} marker count was ${_markers}, expected 1")
            else()
                get_property(_prior GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_COUNT:${_command}")
                if(NOT _prior)
                    set(_prior 0)
                endif()
                math(EXPR _prior "${_prior} + 1")
                set_property(GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_COUNT:${_command}" "${_prior}")
                set_property(GLOBAL PROPERTY "SPARK_OBJECTIVE_${phase}_OUT:${_command}#${_prior}" "${_output}")
                set_property(GLOBAL APPEND PROPERTY "SPARK_OBJECTIVE_${phase}_ORDER" "${_command}#${_prior}")
                list(APPEND _seen_markers "${_marker}")
            endif()
        endif()
    endmacro()

    foreach(_line IN LISTS _lines)
        if(NOT _ok)
            break()
        endif()
        if(_line MATCHES "^frame ")
            _spark_objective_close_block()
            if(NOT _ok)
                break()
            endif()
            if(NOT _line MATCHES "${_spark_objective_header_regex}")
                set(_ok FALSE)
                set(_reason "malformed audit header '${_line}'")
                break()
            endif()
            set(_command "${CMAKE_MATCH_4}")
            if(NOT CMAKE_MATCH_3 STREQUAL "ok ")
                set(_ok FALSE)
                set(_reason "command '${_command}' has an ERR dispatch header")
                break()
            endif()
            list(LENGTH _seen_commands _entry)
            set(_marker "    > [exec] frame ${CMAKE_MATCH_1} (t=${CMAKE_MATCH_2}s, entry=${_entry}): ${_command}")
            list(APPEND _seen_commands "${_command}")
            set(_in_block TRUE)
            set(_markers 0)
            set(_output "")
            set(_have_output FALSE)
        elseif(_in_block)
            if(_line STREQUAL _marker)
                math(EXPR _markers "${_markers} + 1")
                # Anything before the marker belongs to earlier commands.
                set(_output "")
                set(_have_output FALSE)
            elseif(_line MATCHES "^    > \\[exec\\] frame ")
                # Only previously validated markers BEFORE this occurrence are
                # history. Any other marker is an extra or misidentified run.
                if(_markers GREATER 0 OR NOT _line IN_LIST _seen_markers)
                    set(_ok FALSE)
                    set(_reason "unexpected exec marker in entry ${_entry}: ${_line}")
                endif()
            elseif(_markers GREATER 0)
                if(_have_output)
                    string(APPEND _output "\n${_line}")
                else()
                    set(_output "${_line}")
                    set(_have_output TRUE)
                endif()
            endif()
        endif()
    endforeach()
    if(_ok)
        _spark_objective_close_block()
    endif()

    if(_ok AND NOT "${_seen_commands}" STREQUAL "${expected_commands}")
        list(LENGTH _seen_commands _seen_count)
        list(LENGTH expected_commands _expected_count)
        set(_ok FALSE)
        set(_reason "audit ran ${_seen_count} commands, expected the ${_expected_count} scripted commands in order")
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

# Validate one finished phase against the lifecycle contract and the spec rules.
function(_spark_objective_check_phase phase child_result child_stdout child_stderr audit out_ok out_reason)
    _spark_validate_headless_nullrhi_result("${child_result}" "${child_stdout}" "${child_stderr}" _ok _reason)
    if(NOT _ok)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "lifecycle: ${_reason}" PARENT_SCOPE)
        return()
    endif()

    _spark_objective_script_entries("${SPARK_OBJECTIVE_${phase}_SCRIPT}" _commands _due_ms)
    _spark_objective_parse_audit("${phase}" "${audit}" "${_commands}" _ok _reason)

    if(_ok)
        foreach(_kind IN ITEMS EXPECT REJECT)
            foreach(_rule IN LISTS SPARK_OBJECTIVE_${phase}_${_kind})
                if(NOT _rule MATCHES "^([^|]+)\\|(.+)$")
                    set(_ok FALSE)
                    set(_reason "malformed ${_kind} rule '${_rule}'")
                    break()
                endif()
                set(_selector "${CMAKE_MATCH_1}")
                set(_regex "${CMAKE_MATCH_2}")
                _spark_objective_select("${phase}" "${_selector}" _found _output)
                if(NOT _found)
                    set(_ok FALSE)
                    set(_reason "${_kind} rule names no audit block '${_selector}'")
                    break()
                endif()
                string(REGEX MATCH "${_regex}" _match "${_output}")
                if(_kind STREQUAL "EXPECT" AND "${_match}" STREQUAL "")
                    set(_ok FALSE)
                    set(_reason "block '${_selector}' does not match '${_regex}':\n${_output}")
                    break()
                elseif(_kind STREQUAL "REJECT" AND NOT "${_match}" STREQUAL "")
                    set(_ok FALSE)
                    set(_reason "block '${_selector}' matches rejected '${_regex}':\n${_output}")
                    break()
                endif()
            endforeach()
            if(NOT _ok)
                break()
            endif()
        endforeach()
    endif()

    if(_ok)
        foreach(_rule IN LISTS SPARK_OBJECTIVE_${phase}_SAME_AS)
            string(REPLACE "|" ";" _fields "${_rule}")
            list(LENGTH _fields _field_count)
            if(_field_count LESS 2 OR _field_count GREATER 3)
                set(_ok FALSE)
                set(_reason "malformed SAME_AS rule '${_rule}'")
                break()
            endif()
            list(GET _fields 0 _selector)
            list(GET _fields 1 _other_phase)
            set(_other_selector "${_selector}")
            if(_field_count EQUAL 3)
                list(GET _fields 2 _other_selector)
            endif()
            list(FIND SPARK_OBJECTIVE_COMPLETED_PHASES "${_other_phase}" _other_position)
            if(_other_position EQUAL -1)
                set(_ok FALSE)
                set(_reason "SAME_AS rule '${_rule}' names phase '${_other_phase}', which has not run before '${phase}'")
                break()
            endif()
            _spark_objective_select("${phase}" "${_selector}" _found _output)
            _spark_objective_select("${_other_phase}" "${_other_selector}" _other_found _other_output)
            if(NOT _found OR NOT _other_found)
                set(_ok FALSE)
                set(_reason "SAME_AS rule '${_rule}' names a missing audit block")
                break()
            endif()
            if(NOT _output STREQUAL _other_output)
                set(_ok FALSE)
                string(CONCAT _reason "block '${_selector}' differs from ${_other_phase} '${_other_selector}':\n"
                       "--- ${phase}\n${_output}\n--- ${_other_phase}\n${_other_output}")
                break()
            endif()
        endforeach()
    endif()

    if(_ok AND COMMAND spark_objective_verify_${phase})
        cmake_language(CALL spark_objective_verify_${phase} "${phase}" _ok _reason)
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

# Whether TEXT names a FORBIDDEN root (source or build tree) once every mention of
# an ALLOWED root (the run's own install and user root, which may lie inside the
# build tree) is removed. Both slash styles are searched; Windows compares
# case-insensitively.
function(_spark_objective_tree_leak text allowed forbidden out_leak)
    set(_scrubbed "${text}")
    set(_needles_allowed)
    set(_needles_forbidden)
    foreach(_kind IN ITEMS allowed forbidden)
        foreach(_root IN LISTS ${_kind})
            set(_forms "${_root}")
            if(EXISTS "${_root}")
                file(REAL_PATH "${_root}" _real)
                list(APPEND _forms "${_real}")
            endif()
            foreach(_form IN LISTS _forms)
                string(REPLACE "\\" "/" _slashed "${_form}")
                string(REPLACE "/" "\\" _backslashed "${_slashed}")
                list(APPEND _needles_${_kind} "${_slashed}" "${_backslashed}")
            endforeach()
        endforeach()
    endforeach()
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${_scrubbed}" _scrubbed)
        string(TOLOWER "${_needles_allowed}" _needles_allowed)
        string(TOLOWER "${_needles_forbidden}" _needles_forbidden)
    endif()
    foreach(_needle IN LISTS _needles_allowed)
        string(REPLACE "${_needle}" "<run-root>" _scrubbed "${_scrubbed}")
    endforeach()
    foreach(_needle IN LISTS _needles_forbidden)
        string(FIND "${_scrubbed}" "${_needle}" _position)
        if(NOT _position EQUAL -1)
            string(SUBSTRING "${_scrubbed}" ${_position} 160 _context)
            set(${out_leak} "${_context}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_leak} "" PARENT_SCOPE)
endfunction()

if(SPARK_MODULE_OBJECTIVE_PARSER_SELF_TEST)
    set(_ready "SPARK_MODULE_READY count=1\n")
    set(_rhi "SPARK_HEADLESS_RHI backend=null initialized=1 frames=90 shutdown=1\n")
    set(_lifecycle "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=90 fixed=90 rendered=0 unloaded=1 faults=0\n")
    set(_stdout_ok "${_ready}${_rhi}${_lifecycle}")

    # Each case runs as its own "phase" name so stored blocks never leak between cases.
    function(_spark_expect_objective_case name result stdout audit expected_ok)
        _spark_objective_check_phase("${name}" "${result}" "${stdout}" "" "${audit}" _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Module objective parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Module objective parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    # The status output of 'probe_status' carries a multi-line console entry,
    # and the window of the second command still shows the first one's output.
    set(_script "1 probe_status\nt1.5 probe_act go\n# comment\n\nt2 probe_status\n")
    set(_audit [=[frame 1 t=0.0s | ok  | probe_status
    > boot line
    > [exec] frame 1 (t=0.0s, entry=0): probe_status
    > Probe: idle
Score: 0
frame 90 t=1.5s | ok  | probe_act go
    > Probe: idle
Score: 0
    > [exec] frame 90 (t=1.5s, entry=1): probe_act go
    > Acted: go
frame 120 t=2.0s | ok  | probe_status
    > Acted: go
    > [exec] frame 120 (t=2.0s, entry=2): probe_status
    > Probe: done
Score: 7
]=])

    macro(_spark_objective_fixture name)
        set(SPARK_OBJECTIVE_${name}_SCRIPT "${_script}")
        set(SPARK_OBJECTIVE_${name}_EXPECT "probe_status|Probe: done\nScore: 7" "probe_act go|> Acted: go$"
                                           "probe_status#1|Score: 0")
        set(SPARK_OBJECTIVE_${name}_REJECT "probe_status|Probe: idle")
        set(SPARK_OBJECTIVE_${name}_SAME_AS "")
    endmacro()

    _spark_objective_fixture(pass)
    _spark_expect_objective_case(pass 0 "${_stdout_ok}" "${_audit}" TRUE)

    string(REPLACE "\n" "\r\n" _audit_crlf "${_audit}")
    _spark_objective_fixture(crlf)
    _spark_expect_objective_case(crlf 0 "${_stdout_ok}" "${_audit_crlf}" TRUE)

    _spark_objective_fixture(nonzero_exit)
    _spark_expect_objective_case(nonzero_exit 3 "${_stdout_ok}" "${_audit}" FALSE)

    _spark_objective_fixture(lifecycle_fault)
    string(REPLACE "faults=0" "faults=1" _stdout_fault "${_stdout_ok}")
    _spark_expect_objective_case(lifecycle_fault 0 "${_stdout_fault}" "${_audit}" FALSE)

    _spark_objective_fixture(err_header)
    string(REPLACE "| ok  | probe_act go" "| ERR | probe_act go" _audit_err "${_audit}")
    _spark_expect_objective_case(err_header 0 "${_stdout_ok}" "${_audit_err}" FALSE)

    _spark_objective_fixture(malformed_header)
    _spark_expect_objective_case(malformed_header 0 "${_stdout_ok}" "frame x\n${_audit}" FALSE)

    # The script's last command never ran: the time limit cut the run short.
    _spark_objective_fixture(truncated_script)
    string(FIND "${_audit}" "frame 120 t=2.0s" _cut)
    string(SUBSTRING "${_audit}" 0 ${_cut} _audit_truncated)
    set(SPARK_OBJECTIVE_truncated_script_EXPECT "probe_act go|Acted")
    set(SPARK_OBJECTIVE_truncated_script_REJECT "")
    _spark_expect_objective_case(truncated_script 0 "${_stdout_ok}" "${_audit_truncated}" FALSE)

    _spark_objective_fixture(missing_marker)
    string(REPLACE "    > [exec] frame 90 (t=1.5s, entry=1): probe_act go\n" "" _audit_nomarker "${_audit}")
    _spark_expect_objective_case(missing_marker 0 "${_stdout_ok}" "${_audit_nomarker}" FALSE)

    # A hitch releases two scheduled occurrences of the SAME command in one
    # frame. AppendAudit's history window repeats occurrence 0 in block 1.
    set(_catchup_script "t0.1 probe_act go\nt0.2 probe_act go\n")
    set(_catchup_first [=[frame 12 t=0.3s | ok  | probe_act go
    > [exec] frame 12 (t=0.3s, entry=0): probe_act go
    > Acted: first
]=])
    set(_catchup_second [=[frame 12 t=0.3s | ok  | probe_act go
    > [exec] frame 12 (t=0.3s, entry=0): probe_act go
    > Acted: first
    > [exec] frame 12 (t=0.3s, entry=1): probe_act go
    > Acted: second
]=])
    set(SPARK_OBJECTIVE_catchup_SCRIPT "${_catchup_script}")
    set(SPARK_OBJECTIVE_catchup_EXPECT "probe_act go#1|^    > Acted: first$"
                                         "probe_act go#2|^    > Acted: second$")
    _spark_expect_objective_case(catchup 0 "${_stdout_ok}" "${_catchup_first}${_catchup_second}" TRUE)

    # Equal scheduled times also have distinct indices; no time string is used
    # as occurrence identity. Rules still select each occurrence's own output.
    set(SPARK_OBJECTIVE_tied_SCRIPT "t0.1 probe_act go\nt0.1 probe_act go\n")
    set(SPARK_OBJECTIVE_tied_EXPECT "${SPARK_OBJECTIVE_catchup_EXPECT}")
    _spark_expect_objective_case(tied 0 "${_stdout_ok}" "${_catchup_first}${_catchup_second}" TRUE)

    set(_second_marker "    > [exec] frame 12 (t=0.3s, entry=1): probe_act go")
    set(SPARK_OBJECTIVE_duplicate_marker_SCRIPT "${_catchup_script}")
    string(REPLACE "${_second_marker}" "${_second_marker}\n${_second_marker}" _duplicate "${_catchup_second}")
    _spark_expect_objective_case(duplicate_marker 0 "${_stdout_ok}" "${_catchup_first}${_duplicate}" FALSE)

    # Two headers cannot claim the same occurrence, even when their text and
    # total count match the script. An extra block cannot pass either.
    set(SPARK_OBJECTIVE_duplicate_occurrence_SCRIPT "${_catchup_script}")
    _spark_expect_objective_case(duplicate_occurrence 0 "${_stdout_ok}" "${_catchup_first}${_catchup_first}" FALSE)
    set(SPARK_OBJECTIVE_extra_occurrence_SCRIPT "${_catchup_script}")
    string(REPLACE "entry=1" "entry=2" _extra "${_catchup_second}")
    _spark_expect_objective_case(extra_occurrence 0 "${_stdout_ok}"
        "${_catchup_first}${_catchup_second}${_extra}" FALSE)

    set(SPARK_OBJECTIVE_missing_occurrence_SCRIPT "${_catchup_script}")
    _spark_expect_objective_case(missing_occurrence 0 "${_stdout_ok}" "${_catchup_first}" FALSE)
    set(SPARK_OBJECTIVE_missing_catchup_marker_SCRIPT "${_catchup_script}")
    string(REPLACE "${_second_marker}\n" "" _missing "${_catchup_second}")
    _spark_expect_objective_case(missing_catchup_marker 0 "${_stdout_ok}" "${_catchup_first}${_missing}" FALSE)

    set(SPARK_OBJECTIVE_wrong_occurrence_SCRIPT "${_catchup_script}")
    string(REPLACE "entry=1" "entry=9" _wrong "${_catchup_second}")
    _spark_expect_objective_case(wrong_occurrence 0 "${_stdout_ok}" "${_catchup_first}${_wrong}" FALSE)
    set(SPARK_OBJECTIVE_extra_marker_SCRIPT "${_catchup_script}")
    _spark_expect_objective_case(extra_marker 0 "${_stdout_ok}"
        "${_catchup_first}${_catchup_second}    > [exec] frame 12 (t=0.3s, entry=9): probe_act go\n" FALSE)

    # A previous occurrence's output in the same frame must not satisfy a rule.
    set(SPARK_OBJECTIVE_stale_catchup_output_SCRIPT "${_catchup_script}")
    set(SPARK_OBJECTIVE_stale_catchup_output_EXPECT "probe_act go#2|Acted: first")
    _spark_expect_objective_case(stale_catchup_output 0 "${_stdout_ok}"
        "${_catchup_first}${_catchup_second}" FALSE)

    # 'Probe: done' appears only in the first probe_status block; the rule reads the last.
    _spark_objective_fixture(non_final_block)
    set(SPARK_OBJECTIVE_non_final_block_EXPECT "probe_status|Score: 0")
    set(SPARK_OBJECTIVE_non_final_block_REJECT "")
    _spark_expect_objective_case(non_final_block 0 "${_stdout_ok}" "${_audit}" FALSE)

    # 'Score: 0' is in probe_act's console window, but before its marker.
    _spark_objective_fixture(other_command_block)
    set(SPARK_OBJECTIVE_other_command_block_EXPECT "probe_act go|Score: 0")
    set(SPARK_OBJECTIVE_other_command_block_REJECT "")
    _spark_expect_objective_case(other_command_block 0 "${_stdout_ok}" "${_audit}" FALSE)

    _spark_objective_fixture(unknown_selector)
    set(SPARK_OBJECTIVE_unknown_selector_EXPECT "probe_status#3|Probe")
    _spark_expect_objective_case(unknown_selector 0 "${_stdout_ok}" "${_audit}" FALSE)

    _spark_objective_fixture(rejected_match)
    set(SPARK_OBJECTIVE_rejected_match_REJECT "probe_status|Score: 7")
    _spark_expect_objective_case(rejected_match 0 "${_stdout_ok}" "${_audit}" FALSE)

    # Cross-process rules: the reader restores the writer's final status block.
    set(SPARK_OBJECTIVE_COMPLETED_PHASES pass)
    _spark_objective_fixture(same_as_equal)
    set(SPARK_OBJECTIVE_same_as_equal_SAME_AS "probe_status|pass" "probe_status#1|pass|probe_status#1")
    _spark_expect_objective_case(same_as_equal 0 "${_stdout_ok}" "${_audit}" TRUE)

    _spark_objective_fixture(same_as_mismatch)
    string(REPLACE "Score: 7" "Score: 8" _audit_mismatch "${_audit}")
    set(SPARK_OBJECTIVE_same_as_mismatch_EXPECT "")
    set(SPARK_OBJECTIVE_same_as_mismatch_SAME_AS "probe_status|pass")
    _spark_expect_objective_case(same_as_mismatch 0 "${_stdout_ok}" "${_audit_mismatch}" FALSE)

    _spark_objective_fixture(same_as_later_phase)
    set(SPARK_OBJECTIVE_same_as_later_phase_SAME_AS "probe_status|not_run_yet")
    _spark_expect_objective_case(same_as_later_phase 0 "${_stdout_ok}" "${_audit}" FALSE)

    # A spec hook sees every block in audit order, and its verdict decides the phase.
    function(spark_objective_verify_custom_hook_pass phase out_ok out_reason)
        spark_objective_block_order("${phase}" _order)
        spark_objective_block_output("${phase}" "probe_act go" 1 _acted)
        if("${_order}" STREQUAL "probe_status#1;probe_act go#1;probe_status#2" AND _acted STREQUAL "    > Acted: go")
            set(${out_ok} TRUE PARENT_SCOPE)
        else()
            set(${out_ok} FALSE PARENT_SCOPE)
            set(${out_reason} "hook saw order '${_order}' and output '${_acted}'" PARENT_SCOPE)
        endif()
    endfunction()
    _spark_objective_fixture(custom_hook_pass)
    _spark_expect_objective_case(custom_hook_pass 0 "${_stdout_ok}" "${_audit}" TRUE)

    function(spark_objective_verify_custom_hook_reject phase out_ok out_reason)
        spark_objective_block_count("${phase}" "probe_status" _count)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "custom hook rejected ${_count} blocks" PARENT_SCOPE)
    endfunction()
    _spark_objective_fixture(custom_hook_reject)
    _spark_expect_objective_case(custom_hook_reject 0 "${_stdout_ok}" "${_audit}" FALSE)

    # Tree leaks: the run root may sit inside the build tree and is scrubbed first.
    function(_spark_expect_leak_case name text expected_leak)
        _spark_objective_tree_leak("${text}" "/work/build/runs/r1" "/work/src;/work/build" _leak)
        if(expected_leak AND _leak STREQUAL "")
            message(FATAL_ERROR "Tree-leak case '${name}' found no leak")
        elseif(NOT expected_leak AND NOT _leak STREQUAL "")
            message(FATAL_ERROR "Tree-leak case '${name}' reported a leak: ${_leak}")
        endif()
    endfunction()
    _spark_expect_leak_case(run_root_only "Loaded /work/build/runs/r1/install/bin/libSparkGame.so" FALSE)
    _spark_expect_leak_case(build_tree "Loaded /work/build/bin/libSparkGame.so" TRUE)
    _spark_expect_leak_case(source_tree "Reading /work/src/Assets/Localization/x.json" TRUE)
    _spark_expect_leak_case(backslash_source "Reading \\work\\src\\Assets" TRUE)

    # Spec lint: a well-formed spec passes; each defect is refused before any process would run.
    function(_spark_expect_lint_case name expected_ok)
        _spark_objective_lint_spec(_actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Objective spec lint case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Objective spec lint case '${name}' unexpectedly passed")
        endif()
    endfunction()
    macro(_spark_lint_fixture)
        set(SPARK_OBJECTIVE_PHASES first second)
        set(SPARK_OBJECTIVE_first_SCRIPT "${_script}")
        set(SPARK_OBJECTIVE_first_SECONDS 3)
        set(SPARK_OBJECTIVE_first_EXPECT "probe_status#2|Probe: done")
        set(SPARK_OBJECTIVE_first_REJECT "probe_status#1|Probe: done")
        set(SPARK_OBJECTIVE_first_SAME_AS "")
        set(SPARK_OBJECTIVE_second_SCRIPT "1 probe_status\n")
        set(SPARK_OBJECTIVE_second_SECONDS 2)
        set(SPARK_OBJECTIVE_second_EXPECT "")
        set(SPARK_OBJECTIVE_second_REJECT "")
        set(SPARK_OBJECTIVE_second_SAME_AS "probe_status|first|probe_status#2")
    endmacro()
    _spark_lint_fixture()
    _spark_expect_lint_case(lint_valid TRUE)
    set(SPARK_OBJECTIVE_first_SECONDS 2)
    _spark_expect_lint_case(lint_command_at_limit FALSE)
    _spark_lint_fixture()
    set(SPARK_OBJECTIVE_first_SCRIPT "t2 probe_status\n1 probe_act go\n")
    _spark_expect_lint_case(lint_out_of_order FALSE)
    _spark_lint_fixture()
    set(SPARK_OBJECTIVE_first_EXPECT "probe_status#3|Probe")
    _spark_expect_lint_case(lint_unscripted_selector FALSE)
    _spark_lint_fixture()
    set(SPARK_OBJECTIVE_second_SAME_AS "probe_status|second")
    _spark_expect_lint_case(lint_same_as_own_phase FALSE)
    _spark_lint_fixture()
    set(SPARK_OBJECTIVE_second_SECONDS 1.5)
    _spark_expect_lint_case(lint_fractional_seconds FALSE)

    # Every committed per-module spec must lint cleanly.
    function(_spark_lint_committed_spec spec)
        include("${spec}")
        _spark_objective_lint_spec(_spec_ok _spec_reason)
        if(NOT _spec_ok)
            message(FATAL_ERROR "Objective spec ${spec} is malformed: ${_spec_reason}")
        endif()
    endfunction()
    file(GLOB _committed_specs "${CMAKE_CURRENT_LIST_DIR}/ModuleObjectives/*.cmake")
    foreach(_spec IN LISTS _committed_specs)
        _spark_lint_committed_spec("${_spec}")
    endforeach()
    list(LENGTH _committed_specs _committed_spec_count)

    message(STATUS "Installed module objective audit parser contract passed "
                   "(${_committed_spec_count} committed module specs lint cleanly)")
    return()
endif()

foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
                           SPARK_MODULE_TARGET SPARK_MODULE_FILE_NAME SPARK_OBJECTIVE_SPEC)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunInstalledModuleObjective.cmake requires -D${_required}=<value>")
    endif()
endforeach()
if(NOT SPARK_MODULE_TARGET MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR SPARK_MODULE_FILE_NAME MATCHES "[/\\\\;]")
    message(FATAL_ERROR "SPARK_MODULE_TARGET/SPARK_MODULE_FILE_NAME are not plain names")
endif()
if(NOT EXISTS "${SPARK_OBJECTIVE_SPEC}" OR IS_DIRECTORY "${SPARK_OBJECTIVE_SPEC}")
    message(FATAL_ERROR "Objective spec is missing: ${SPARK_OBJECTIVE_SPEC}")
endif()

# Bind the evidence to the source revision (same rules as RunInstalledFPSPackage.cmake).
find_program(_git_executable NAMES git git.exe REQUIRED)
execute_process(
    COMMAND "${_git_executable}" -C "${SPARK_SOURCE_ROOT}" rev-parse HEAD
    RESULT_VARIABLE _git_head_result
    OUTPUT_VARIABLE _source_sha
    ERROR_VARIABLE _git_head_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 30)
string(LENGTH "${_source_sha}" _source_sha_length)
if(NOT _git_head_result EQUAL 0 OR NOT _source_sha_length EQUAL 40 OR _source_sha MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "Could not bind module objective evidence to source HEAD: ${_git_head_error}")
endif()
execute_process(
    COMMAND "${_git_executable}" -C "${SPARK_SOURCE_ROOT}"
        status --porcelain=v1 --untracked-files=no
    RESULT_VARIABLE _git_status_result
    OUTPUT_VARIABLE _git_status_output
    ERROR_VARIABLE _git_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 30)
if(NOT _git_status_result EQUAL 0)
    message(FATAL_ERROR "Could not inspect module objective source state: ${_git_status_error}")
endif()
if(_git_status_output STREQUAL "")
    set(_source_tree_state clean)
else()
    set(_source_tree_state dirty)
endif()

function(_run_checked _stage _timeout)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT ${_timeout})
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_stage} failed (${_result}):\n${_output}\n${_error}")
    endif()
endfunction()

# Harden the test root (same rules as RunInstalledFPSPackage.cmake).
if(NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR "${SPARK_TEST_ROOT}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()
cmake_path(SET _test_root NORMALIZE "${SPARK_TEST_ROOT}")
cmake_path(GET _test_root ROOT_PATH _test_volume_root)
if(_test_root STREQUAL _test_volume_root)
    message(FATAL_ERROR "SPARK_TEST_ROOT must not be a volume root")
endif()
if(EXISTS "${_test_root}" AND (NOT IS_DIRECTORY "${_test_root}" OR IS_SYMLINK "${_test_root}"))
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a regular directory when it exists")
endif()
file(MAKE_DIRECTORY "${_test_root}")
file(REAL_PATH "${_test_root}" _test_root_real)
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_test_root}" _test_root_compare)
    string(TOLOWER "${_test_root_real}" _test_root_real_compare)
else()
    set(_test_root_compare "${_test_root}")
    set(_test_root_real_compare "${_test_root_real}")
endif()
if(NOT _test_root_compare STREQUAL _test_root_real_compare)
    message(FATAL_ERROR "SPARK_TEST_ROOT must not cross a symlink or reparse point")
endif()

string(TIMESTAMP _run_timestamp "%Y%m%dT%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_nonce)
set(_run_root "${_test_root}/run-${_run_timestamp}-${_run_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated module objective run root already exists: ${_run_root}")
endif()
file(MAKE_DIRECTORY "${_run_root}")
set(_install_root "${_run_root}/install")

_run_checked("Install configured runtime component" 600
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component runtime)
_run_checked("Install configured game-module component" 300
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component samples)

set(_bin "${_install_root}/bin")
if(CMAKE_HOST_WIN32)
    set(_engine "${_bin}/SparkEngine.exe")
else()
    set(_engine "${_bin}/SparkEngine")
endif()
set(_module "${_bin}/${SPARK_MODULE_FILE_NAME}")
foreach(_required_file IN ITEMS "${_engine}" "${_module}" "${_module}.sparkabi" "${_bin}/Assets/assets.integrity.json")
    if(NOT EXISTS "${_required_file}" OR IS_DIRECTORY "${_required_file}" OR IS_SYMLINK "${_required_file}")
        message(FATAL_ERROR "Installed module objective input is missing or unsafe: ${_required_file}")
    endif()
endforeach()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
_run_checked("Check ${SPARK_MODULE_TARGET} asset references" 120
    "${Python3_EXECUTABLE}" -B "${SPARK_SOURCE_ROOT}/tools/check-module-asset-refs.py"
    --module "${SPARK_MODULE_TARGET}")
_run_checked("Verify installed asset integrity" 300
    "${Python3_EXECUTABLE}" -B "${SPARK_SOURCE_ROOT}/tools/asset-integrity/verify_asset_integrity.py"
    verify "${_bin}/Assets/assets.integrity.json" --root "${_bin}/Assets")

# One isolated user root for every phase, so saves carry between processes and
# nothing from the developer's own profile can be read (RunSparkHeadlessFPSSaveReload.cmake).
set(_user_root "${_run_root}/user")
if(CMAKE_HOST_WIN32)
    set(_user_env
        "LOCALAPPDATA=${_user_root}/localappdata"
        "APPDATA=${_user_root}/appdata")
else()
    set(_user_env
        "HOME=${_user_root}/home"
        "XDG_DATA_HOME=${_user_root}/data"
        "XDG_CONFIG_HOME=${_user_root}/config"
        "XDG_CACHE_HOME=${_user_root}/cache"
        "XDG_STATE_HOME=${_user_root}/state")
endif()
foreach(_assignment IN LISTS _user_env)
    string(REGEX REPLACE "^[A-Z_]+=" "" _dir "${_assignment}")
    file(MAKE_DIRECTORY "${_dir}")
endforeach()
if(CMAKE_HOST_WIN32)
    set(_launcher "${CMAKE_COMMAND}" -E env)
else()
    # Nothing from the caller's environment (LD_LIBRARY_PATH, SPARK_* overrides) reaches the installed run.
    find_program(_env_program NAMES env REQUIRED)
    file(MAKE_DIRECTORY "${_run_root}/tmp")
    set(_launcher "${_env_program}" -i "PATH=/usr/bin:/bin" "LC_ALL=C" "TMPDIR=${_run_root}/tmp")
endif()

include("${SPARK_OBJECTIVE_SPEC}")
_spark_objective_lint_spec(_spec_ok _spec_reason)
if(NOT _spec_ok)
    message(FATAL_ERROR "Objective spec ${SPARK_OBJECTIVE_SPEC} is malformed: ${_spec_reason}")
endif()

set(SPARK_OBJECTIVE_COMPLETED_PHASES)
foreach(_phase IN LISTS SPARK_OBJECTIVE_PHASES)
    set(_script "${_run_root}/${_phase}-exec.txt")
    set(_audit "${_run_root}/${_phase}-exec-audit.log")
    file(WRITE "${_script}" "${SPARK_OBJECTIVE_${_phase}_SCRIPT}")
    if(EXISTS "${_audit}")
        message(FATAL_ERROR "Could not establish a fresh exec audit before phase ${_phase}")
    endif()
    math(EXPR _timeout "${SPARK_OBJECTIVE_${_phase}_SECONDS} + 120")

    message(STATUS "Module objective ${SPARK_MODULE_TARGET}: phase ${_phase} "
                   "(${SPARK_OBJECTIVE_${_phase}_SECONDS} s wall clock)")
    execute_process(
        COMMAND ${_launcher}
            ${_user_env}
            "SPARK_RHI_BACKEND=null"
            "${_engine}"
            -headless
            -game "${_module}"
            -require-game
            -exec "${_script}"
            -exec-audit "${_audit}"
            -test-seconds "${SPARK_OBJECTIVE_${_phase}_SECONDS}"
            -threads 2
            -no-subprocess
        WORKING_DIRECTORY "${_bin}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT ${_timeout}
        ENCODING UTF-8)
    file(WRITE "${_run_root}/${_phase}-stdout.log" "${_stdout}")
    file(WRITE "${_run_root}/${_phase}-stderr.log" "${_stderr}")

    set(_audit_content "")
    if(EXISTS "${_audit}" AND NOT IS_DIRECTORY "${_audit}" AND NOT IS_SYMLINK "${_audit}")
        file(READ "${_audit}" _audit_content)
    endif()
    _spark_objective_check_phase("${_phase}" "${_result}" "${_stdout}" "${_stderr}" "${_audit_content}"
        _phase_ok _phase_reason)
    if(_phase_ok)
        _spark_objective_tree_leak("${_stdout}\n${_stderr}\n${_audit_content}" "${_run_root}"
            "${SPARK_SOURCE_ROOT};${SPARK_ENGINE_BUILD_DIR}" _leak)
        if(NOT _leak STREQUAL "")
            set(_phase_ok FALSE)
            set(_phase_reason "output names the source or build tree: ${_leak}")
        endif()
    endif()
    if(NOT _phase_ok)
        message(FATAL_ERROR
            "Installed ${SPARK_MODULE_TARGET} objective phase '${_phase}' failed: ${_phase_reason}\n"
            "Evidence kept under ${_run_root} (${_phase}-stdout.log, ${_phase}-stderr.log, ${_phase}-exec-audit.log)")
    endif()
    list(APPEND SPARK_OBJECTIVE_COMPLETED_PHASES "${_phase}")
endforeach()

file(SHA256 "${_engine}" _engine_sha256)
file(SHA256 "${_module}" _module_sha256)
file(SHA256 "${SPARK_OBJECTIVE_SPEC}" _spec_sha256)
file(SHA256 "${_runner_script}" _runner_sha256)
file(WRITE "${_run_root}/evidence.txt"
    "source_sha=${_source_sha}\n"
    "source_tree_state=${_source_tree_state}\n"
    "build_config=${SPARK_CONFIG}\n"
    "host_system=${CMAKE_HOST_SYSTEM_NAME}\n"
    "module=${SPARK_MODULE_TARGET}\n"
    "engine_sha256=${_engine_sha256}\n"
    "module_sha256=${_module_sha256}\n"
    "spec_sha256=${_spec_sha256}\n"
    "runner_sha256=${_runner_sha256}\n"
    "phases=${SPARK_OBJECTIVE_COMPLETED_PHASES}\n"
    "backend=null\n")
message(STATUS
    "Installed ${SPARK_MODULE_TARGET} objective passed (${SPARK_OBJECTIVE_COMPLETED_PHASES}) at "
    "${_source_sha} (${_source_tree_state}, ${SPARK_CONFIG}); evidence under ${_run_root}")
