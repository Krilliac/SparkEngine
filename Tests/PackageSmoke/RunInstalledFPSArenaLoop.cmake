cmake_minimum_required(VERSION 3.25)

# MOD-310: play the single-player arena loop in an installed SparkGameFPS
# package. The installed SparkEngine.exe runs the installed module on D3D11/WARP
# from an empty working directory with an empty LOCALAPPDATA; the developer
# command `fps_autoplay on` hands the player to FPSArenaAutopilot, which moves
# through the real input path and fires the real weapon. game_status prints the
# production scoreboard as
#
#   Loop: kills=K deaths=D respawns=R score=S moved=M autopilot=<off|hunt|yield|complete>
#
# and the run passes only when the last such line reports kills>=2, deaths>=1,
# respawns>=1, score>0, moved>=2.0 m and autopilot=complete, every scheduled
# game_status ran, and the module lifecycle closed cleanly on a WARP device.
#
# Inputs: SPARK_INSTALLED_ROOT (the fresh install prefix), SPARK_TEST_ROOT.
# SPARK_FPS_ARENA_LOOP_PARSER_SELF_TEST=ON runs only the verdict contract.
#
# A fresh install prefix approximates a clean Windows install on this host; it
# is not a clean machine.

set(_spark_arena_loop_seconds 150)
set(_spark_arena_loop_status_every 15)
set(_spark_arena_loop_line_regex
    "^Loop: kills=([0-9]+) deaths=([0-9]+) respawns=([0-9]+) score=(-?[0-9]+) moved=([0-9]+)[.]([0-9]) autopilot=(off|hunt|yield|complete)$")

# Verdict over an exec audit trail. Every line that mentions "Loop:" must be a
# well-formed record; the last one decides, so a record that never printed, or
# a run the time limit cut short, cannot pass.
function(_spark_fps_arena_loop_verdict audit expected_status out_ok out_reason out_summary)
    set(${out_ok} FALSE PARENT_SCOPE)
    set(${out_summary} "" PARENT_SCOPE)
    string(REPLACE "\r\n" "\n" _audit "${audit}")
    string(REPLACE ";" "\\;" _audit "${_audit}")
    string(REPLACE "\n" ";" _lines "${_audit}")

    set(_autoplay_headers 0)
    set(_autoplay_replies 0)
    set(_status_headers 0)
    set(_last_record "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^frame [0-9]+ t=[0-9.]+s [|] (ok |ERR) [|] (.*)$")
            set(_verdict "${CMAKE_MATCH_1}")
            set(_command "${CMAKE_MATCH_2}")
            if(_command STREQUAL "fps_autoplay on")
                if(NOT _verdict STREQUAL "ok ")
                    set(${out_reason} "fps_autoplay on was rejected by the console" PARENT_SCOPE)
                    return()
                endif()
                math(EXPR _autoplay_headers "${_autoplay_headers} + 1")
            elseif(_command STREQUAL "game_status")
                if(NOT _verdict STREQUAL "ok ")
                    set(${out_reason} "a scheduled game_status failed" PARENT_SCOPE)
                    return()
                endif()
                math(EXPR _status_headers "${_status_headers} + 1")
            endif()
        elseif(_line STREQUAL "    > Arena autopilot on")
            math(EXPR _autoplay_replies "${_autoplay_replies} + 1")
        elseif(_line MATCHES "Loop: kills=")
            # The audit prefixes only the first line of a logged message with
            # "    > ", and game_status is one multi-line message, so its loop
            # record normally starts the line bare. Anything else before it
            # (a logger prefix, a second record) is not the record game_status prints.
            string(REGEX REPLACE "^    > " "" _record "${_line}")
            if(NOT _record MATCHES "${_spark_arena_loop_line_regex}")
                set(${out_reason} "malformed loop record: '${_record}'" PARENT_SCOPE)
                return()
            endif()
            set(_last_record "${_record}")
        endif()
    endforeach()

    if(NOT _autoplay_headers EQUAL 1 OR NOT _autoplay_replies EQUAL 1)
        set(${out_reason}
            "expected one accepted fps_autoplay on (headers=${_autoplay_headers}, replies=${_autoplay_replies})"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT _status_headers EQUAL expected_status)
        set(${out_reason}
            "ran ${_status_headers} of ${expected_status} scheduled game_status commands (the run was cut short)"
            PARENT_SCOPE)
        return()
    endif()
    if(_last_record STREQUAL "")
        set(${out_reason} "game_status printed no loop record" PARENT_SCOPE)
        return()
    endif()

    string(REGEX MATCH "${_spark_arena_loop_line_regex}" _unused "${_last_record}")
    set(_kills "${CMAKE_MATCH_1}")
    set(_deaths "${CMAKE_MATCH_2}")
    set(_respawns "${CMAKE_MATCH_3}")
    set(_score "${CMAKE_MATCH_4}")
    set(_moved_whole "${CMAKE_MATCH_5}")
    set(_phase "${CMAKE_MATCH_7}")
    if(_kills LESS 2)
        set(${out_reason} "final loop record has kills=${_kills}, expected >= 2: '${_last_record}'" PARENT_SCOPE)
    elseif(_deaths LESS 1)
        set(${out_reason} "final loop record has deaths=${_deaths}, expected >= 1: '${_last_record}'" PARENT_SCOPE)
    elseif(_respawns LESS 1)
        set(${out_reason} "final loop record has respawns=${_respawns}, expected >= 1: '${_last_record}'" PARENT_SCOPE)
    elseif(_score LESS 1)
        set(${out_reason} "final loop record has score=${_score}, expected > 0: '${_last_record}'" PARENT_SCOPE)
    elseif(_moved_whole LESS 2)
        set(${out_reason} "final loop record moved less than 2.0 m: '${_last_record}'" PARENT_SCOPE)
    elseif(NOT _phase STREQUAL "complete")
        set(${out_reason} "final loop record has autopilot=${_phase}, expected complete: '${_last_record}'"
            PARENT_SCOPE)
    else()
        set(${out_ok} TRUE PARENT_SCOPE)
        set(${out_reason} "" PARENT_SCOPE)
        set(${out_summary} "${_last_record}" PARENT_SCOPE)
    endif()
endfunction()

if(SPARK_FPS_ARENA_LOOP_PARSER_SELF_TEST)
    set(_header "frame 1 t=0.0s | ok  | fps_autoplay on\n    > [exec] frame 1 (t=0.0s, entry=0): fps_autoplay on\n")
    set(_reply "    > Arena autopilot on\n")
    set(_status1 "frame 900 t=15.0s | ok  | game_status\n    > === Spark Arena ===\n")
    set(_status2 "frame 1800 t=30.0s | ok  | game_status\n    > === Spark Arena ===\n")
    set(_early "Loop: kills=1 deaths=0 respawns=0 score=100 moved=6.3 autopilot=yield\n")
    set(_final "Loop: kills=2 deaths=1 respawns=1 score=200 moved=6.3 autopilot=complete\n")
    set(_valid "${_header}${_reply}${_status1}${_early}${_status2}${_final}")

    _spark_fps_arena_loop_verdict("${_valid}" 2 _ok _reason _summary)
    if(NOT _ok OR NOT _summary STREQUAL "Loop: kills=2 deaths=1 respawns=1 score=200 moved=6.3 autopilot=complete")
        message(FATAL_ERROR "arena loop parser rejected a complete loop: ${_reason}")
    endif()

    set(_case_names)
    set(_case_audits)
    macro(_spark_arena_loop_case name audit)
        list(APPEND _case_names "${name}")
        string(REPLACE ";" "\\;" _escaped_case "${audit}")
        list(APPEND _case_audits "${_escaped_case}")
    endmacro()
    string(REPLACE "kills=2" "kills=1" _one_kill "${_valid}")
    _spark_arena_loop_case("one kill" "${_one_kill}")
    string(REPLACE "deaths=1" "deaths=0" _no_death "${_valid}")
    _spark_arena_loop_case("no death" "${_no_death}")
    string(REPLACE "respawns=1 score=200" "respawns=0 score=200" _no_respawn "${_valid}")
    _spark_arena_loop_case("no respawn" "${_no_respawn}")
    string(REPLACE "score=200" "score=0" _no_score "${_valid}")
    _spark_arena_loop_case("no score" "${_no_score}")
    string(REPLACE "moved=6.3 autopilot=complete" "moved=1.9 autopilot=complete" _still "${_valid}")
    _spark_arena_loop_case("did not move" "${_still}")
    string(REPLACE "autopilot=complete" "autopilot=hunt" _hunting "${_valid}")
    _spark_arena_loop_case("not complete" "${_hunting}")
    _spark_arena_loop_case("final record missing" "${_header}${_reply}${_status1}${_early}${_status2}")
    _spark_arena_loop_case("no records" "${_header}${_reply}${_status1}${_status2}")
    _spark_arena_loop_case("status cut short" "${_header}${_reply}${_status1}${_final}")
    _spark_arena_loop_case("autoplay never enabled" "${_status1}${_early}${_status2}${_final}")
    string(REPLACE "| ok  | fps_autoplay on" "| ERR | fps_autoplay on" _autoplay_rejected "${_valid}")
    _spark_arena_loop_case("autoplay rejected" "${_autoplay_rejected}")
    string(REPLACE "    > Arena autopilot on\n" "    > Usage: fps_autoplay on|off\n" _autoplay_usage "${_valid}")
    _spark_arena_loop_case("autoplay reply missing" "${_autoplay_usage}")
    string(REPLACE "autopilot=complete\n" "autopilot=complete x=1\n" _trailing "${_valid}")
    _spark_arena_loop_case("trailing field" "${_trailing}")
    string(REPLACE "\nLoop: kills=2" "\n[INFO] Loop: kills=2" _logger_prefixed "${_valid}")
    _spark_arena_loop_case("logger-prefixed record" "${_logger_prefixed}")
    # A status whose loop record is the first line of the logged message carries the audit prefix.
    string(REPLACE "\nLoop: kills=2" "\n    > Loop: kills=2" _first_line_record "${_valid}")
    _spark_fps_arena_loop_verdict("${_first_line_record}" 2 _prefixed_ok _prefixed_reason _prefixed_summary)
    if(NOT _prefixed_ok)
        message(FATAL_ERROR "arena loop parser rejected an audit-prefixed record: ${_prefixed_reason}")
    endif()

    list(LENGTH _case_names _case_count)
    math(EXPR _case_last "${_case_count} - 1")
    foreach(_index RANGE ${_case_last})
        list(GET _case_names ${_index} _name)
        list(GET _case_audits ${_index} _audit)
        _spark_fps_arena_loop_verdict("${_audit}" 2 _case_ok _case_reason _case_summary)
        if(_case_ok)
            message(FATAL_ERROR "arena loop parser accepted the '${_name}' case")
        endif()
        if(_case_reason STREQUAL "")
            message(FATAL_ERROR "arena loop parser rejected the '${_name}' case without a reason")
        endif()
    endforeach()
    message(STATUS "Installed FPS arena loop parser contract passed (${_case_count} rejection cases)")
    return()
endif()

foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR NOT IS_ABSOLUTE "${${_required}}" OR "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()

set(_bin "${SPARK_INSTALLED_ROOT}/bin")
set(_engine "${_bin}/SparkEngine.exe")
set(_module "${_bin}/SparkGameFPS.dll")
foreach(_required_file IN ITEMS "${_engine}" "${_module}" "${_bin}/Assets/Scenes/level1.scene")
    if(NOT EXISTS "${_required_file}" OR IS_DIRECTORY "${_required_file}" OR IS_SYMLINK "${_required_file}")
        message(FATAL_ERROR "Installed FPS arena-loop input is missing or unsafe: ${_required_file}")
    endif()
endforeach()

string(TIMESTAMP _run_timestamp "%Y%m%dT%H%M%SZ" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_nonce)
set(_run_root "${SPARK_TEST_ROOT}/arena-loop-${_run_timestamp}-${_run_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated installed FPS arena-loop run root already exists: ${_run_root}")
endif()
file(MAKE_DIRECTORY "${_run_root}/work" "${_run_root}/localappdata")

# One status every _spark_arena_loop_status_every seconds leaves a progress
# trace; the last one lands before the -test-seconds limit.
set(_script "${_run_root}/arena-loop.exec")
set(_audit "${_run_root}/arena-loop.audit.log")
set(_script_text "1 fps_autoplay on\n")
set(_expected_status 0)
set(_at ${_spark_arena_loop_status_every})
while(_at LESS _spark_arena_loop_seconds)
    string(APPEND _script_text "t${_at} game_status\n")
    math(EXPR _expected_status "${_expected_status} + 1")
    math(EXPR _at "${_at} + ${_spark_arena_loop_status_every}")
endwhile()
file(WRITE "${_script}" "${_script_text}")

set(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY ON)
get_filename_component(_spark_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_spark_source_root}/cmake/RunSparkModuleProfileLifecycle.cmake")
unset(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY)

math(EXPR _process_timeout "${_spark_arena_loop_seconds} + 120")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=d3d11"
        "SPARK_D3D11_DRIVER=warp"
        "LOCALAPPDATA=${_run_root}/localappdata"
        "${_engine}"
        -game "${_module}"
        -require-game
        -test-seconds ${_spark_arena_loop_seconds}
        -threads 2
        -window-size 640x360
        -no-subprocess
        -exec "${_script}"
        -exec-audit "${_audit}"
    WORKING_DIRECTORY "${_run_root}/work"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT ${_process_timeout}
    ENCODING UTF-8)
file(WRITE "${_run_root}/stdout.log" "${_stdout}")
file(WRITE "${_run_root}/stderr.log" "${_stderr}")

_spark_validate_lifecycle_result("${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _lifecycle_reason)
if(NOT _lifecycle_ok)
    message(FATAL_ERROR "Installed FPS arena loop failed its lifecycle: ${_lifecycle_reason}; logs: ${_run_root}")
endif()
if(NOT EXISTS "${_audit}")
    message(FATAL_ERROR "Installed FPS arena loop wrote no exec audit; logs: ${_run_root}")
endif()
file(READ "${_audit}" _audit_text)
_spark_fps_arena_loop_verdict("${_audit_text}" ${_expected_status} _loop_ok _loop_reason _loop_summary)
if(NOT _loop_ok)
    message(FATAL_ERROR "Installed FPS arena loop did not complete: ${_loop_reason}; logs: ${_run_root}")
endif()

file(SHA256 "${_engine}" _engine_sha256)
file(SHA256 "${_module}" _module_sha256)
file(WRITE "${_run_root}/evidence.txt"
    "engine=${_engine}\n"
    "module=${_module}\n"
    "engine_sha256=${_engine_sha256}\n"
    "module_sha256=${_module_sha256}\n"
    "backend=d3d11-warp\n"
    "seconds=${_spark_arena_loop_seconds}\n"
    "loop=${_loop_summary}\n"
    "result=pass\n")
message(STATUS "Installed SparkGameFPS arena loop completed on D3D11/WARP: ${_loop_summary}; evidence under ${_run_root}")
