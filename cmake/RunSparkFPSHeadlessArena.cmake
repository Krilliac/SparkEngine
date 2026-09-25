cmake_minimum_required(VERSION 3.25)

# MOD-310: runner and strict parser for the SparkGameFPS headless arena record.
#
# The production headless host runs the real SparkGameFPS module on NullRHI.
# Besides the shared SPARK_MODULE_READY / SPARK_HEADLESS_RHI /
# SPARK_HEADLESS_LIFECYCLE contract (reused from
# RunSparkHeadlessNullRHILifecycle.cmake), the module must print exactly one
#
#   SPARK_FPS_HEADLESS_ARENA objects=N spawns=S bound=B mode_spawns=M ticks=T match=1
#
# after a real load of the authored arena. The expected node and default-spawn
# counts come from an independent parse of the same scene file, so the record
# cannot pass unless the module actually loaded that scene, bound every
# authored spawn into RespawnSystem and GameMode, and ticked the arena on every
# OnUpdate the host reported.

set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/RunSparkHeadlessNullRHILifecycle.cmake")
unset(SPARK_HEADLESS_NULLRHI_PARSER_ONLY)

# Count the scene nodes and usable default spawns the way SceneManager's
# authored INI loader and RespawnSystem::CollectAuthoredSpawnPoints do: node
# sections are [Object], [Terrain], [SpawnPoint] and [Camera]; a spawn counts
# when its tag is "default" and any priority is a plain integer.
function(_spark_count_authored_arena scene_path out_objects out_spawns)
    file(READ "${scene_path}" _scene_text)
    string(REPLACE "\r\n" "\n" _scene_text "${_scene_text}")
    string(REPLACE "\r" "\n" _scene_text "${_scene_text}")
    string(REPLACE ";" "\\;" _scene_text "${_scene_text}")
    string(REPLACE "\n" ";" _scene_lines "${_scene_text}")
    set(_objects 0)
    set(_spawns 0)
    set(_section "")
    set(_spawn_tag "")
    set(_spawn_priority_ok TRUE)

    # A trailing sentinel section flushes the last spawn point.
    list(APPEND _scene_lines "[__end__]")
    foreach(_raw IN LISTS _scene_lines)
        string(STRIP "${_raw}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^[#;]")
            continue()
        endif()
        if(_line MATCHES "^\\[(.*)\\]$")
            if(_section STREQUAL "SpawnPoint" AND _spawn_tag STREQUAL "default" AND _spawn_priority_ok)
                math(EXPR _spawns "${_spawns} + 1")
            endif()
            string(STRIP "${CMAKE_MATCH_1}" _section)
            set(_spawn_tag "")
            set(_spawn_priority_ok TRUE)
            if(_section MATCHES "^(Object|Terrain|SpawnPoint|Camera)$")
                math(EXPR _objects "${_objects} + 1")
            endif()
        elseif(_section STREQUAL "SpawnPoint" AND _line MATCHES "^([^=]+)=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" _key)
            string(STRIP "${CMAKE_MATCH_2}" _value)
            if(_key STREQUAL "tag")
                set(_spawn_tag "${_value}")
            elseif(_key STREQUAL "priority" AND NOT _value MATCHES "^-?[0-9]+$")
                set(_spawn_priority_ok FALSE)
            endif()
        endif()
    endforeach()

    set(${out_objects} "${_objects}" PARENT_SCOPE)
    set(${out_spawns} "${_spawns}" PARENT_SCOPE)
endfunction()

function(_spark_validate_fps_headless_arena child_result child_stdout child_stderr expected_objects expected_spawns
         out_ok out_reason)
    _spark_validate_headless_nullrhi_result("${child_result}" "${child_stdout}" "${child_stderr}" _ok _reason)

    if(_ok)
        set(_combined "${child_stdout}\n${child_stderr}")
        string(REPLACE "\r\n" "\n" _combined "${_combined}")
        string(REPLACE "\r" "\n" _combined "${_combined}")
        string(REPLACE ";" "\\;" _combined "${_combined}")
        string(REPLACE "\n" ";" _lines "${_combined}")

        set(_arena_records)
        set(_arena_mentions 0)
        set(_order "")
        set(_updated "")
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "SPARK_FPS_HEADLESS_ARENA")
                math(EXPR _arena_mentions "${_arena_mentions} + 1")
            endif()
            if(_line MATCHES "^SPARK_MODULE_READY count=[0-9]+$")
                string(APPEND _order "R")
            elseif(_line MATCHES
                   "^SPARK_FPS_HEADLESS_ARENA objects=[0-9]+ spawns=[0-9]+ bound=[0-9]+ mode_spawns=[0-9]+ ticks=[0-9]+ match=[01]$")
                list(APPEND _arena_records "${_line}")
                string(APPEND _order "A")
            elseif(_line MATCHES "^SPARK_HEADLESS_LIFECYCLE .* updated=([0-9]+) ")
                set(_updated "${CMAKE_MATCH_1}")
                string(APPEND _order "L")
            endif()
        endforeach()

        list(LENGTH _arena_records _arena_count)
        if(NOT _arena_count EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_arena_count} standalone headless arena records, expected exactly 1")
        elseif(NOT _arena_mentions EQUAL 1)
            set(_ok FALSE)
            set(_reason "found ${_arena_mentions} lines mentioning SPARK_FPS_HEADLESS_ARENA, expected exactly 1")
        elseif(NOT _order STREQUAL "RAL")
            set(_ok FALSE)
            set(_reason "arena record was not emitted between module-ready and the lifecycle record (order ${_order})")
        else()
            list(GET _arena_records 0 _arena)
            string(REGEX MATCH
                   "objects=([0-9]+) spawns=([0-9]+) bound=([0-9]+) mode_spawns=([0-9]+) ticks=([0-9]+) match=([01])"
                   _unused "${_arena}")
            set(_objects "${CMAKE_MATCH_1}")
            set(_spawns "${CMAKE_MATCH_2}")
            set(_bound "${CMAKE_MATCH_3}")
            set(_mode_spawns "${CMAKE_MATCH_4}")
            set(_ticks "${CMAKE_MATCH_5}")
            set(_match "${CMAKE_MATCH_6}")
            if(NOT expected_objects GREATER 0 OR NOT expected_spawns GREATER 0)
                set(_ok FALSE)
                set(_reason "the authored scene declares ${expected_objects} nodes and ${expected_spawns} default spawns")
            elseif(NOT _objects EQUAL expected_objects)
                set(_ok FALSE)
                set(_reason "arena loaded ${_objects} scene nodes, the authored scene has ${expected_objects}")
            elseif(NOT _spawns EQUAL expected_spawns)
                set(_ok FALSE)
                set(_reason "arena collected ${_spawns} default spawns, the authored scene has ${expected_spawns}")
            elseif(NOT _bound EQUAL _spawns)
                set(_ok FALSE)
                set(_reason "RespawnSystem bound ${_bound} of ${_spawns} authored spawns")
            elseif(NOT _mode_spawns EQUAL _spawns)
                set(_ok FALSE)
                set(_reason "GameMode holds ${_mode_spawns} of ${_spawns} authored spawns")
            elseif(_ticks LESS 1 OR NOT _ticks EQUAL _updated)
                set(_ok FALSE)
                set(_reason "arena ticked ${_ticks} times but the host reported ${_updated} OnUpdate callbacks")
            elseif(NOT _match STREQUAL "1")
                set(_ok FALSE)
                set(_reason "the Deathmatch match was not active at unload")
            endif()
        endif()
    endif()

    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_FPS_HEADLESS_ARENA_PARSER_SELF_TEST)
    function(_spark_expect_arena_case name stdout expected_ok)
        _spark_validate_fps_headless_arena(0 "${stdout}" "" 72 4 _actual_ok _reason)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Headless arena parser case '${name}' unexpectedly failed: ${_reason}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Headless arena parser case '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_ready "SPARK_MODULE_READY count=1\n")
    set(_arena "SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=8 match=1\n")
    set(_rhi "SPARK_HEADLESS_RHI backend=null initialized=1 frames=8 shutdown=1\n")
    set(_lifecycle
        "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=0 unloaded=1 faults=0\n")
    _spark_expect_arena_case(valid "${_ready}${_arena}${_rhi}${_lifecycle}" TRUE)
    _spark_expect_arena_case(missing-arena "${_ready}${_rhi}${_lifecycle}" FALSE)
    _spark_expect_arena_case(duplicate-arena "${_ready}${_arena}${_arena}${_rhi}${_lifecycle}" FALSE)
    _spark_expect_arena_case(logger-only-arena "${_ready}[info] ${_arena}${_rhi}${_lifecycle}" FALSE)
    _spark_expect_arena_case(logger-copy-of-arena "${_ready}[info] ${_arena}${_arena}${_rhi}${_lifecycle}" FALSE)
    _spark_expect_arena_case(arena-before-ready "${_arena}${_ready}${_rhi}${_lifecycle}" FALSE)
    _spark_expect_arena_case(arena-after-lifecycle "${_ready}${_rhi}${_lifecycle}${_arena}" FALSE)
    _spark_expect_arena_case(broken-lifecycle
        "${_ready}${_arena}${_rhi}SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=1 unloaded=1 faults=0\n"
        FALSE)
    _spark_expect_arena_case(wrong-objects
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=71 spawns=4 bound=4 mode_spawns=4 ticks=8 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(wrong-spawns
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=1 bound=1 mode_spawns=1 ticks=8 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(respawn-unbound
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=0 mode_spawns=4 ticks=8 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(gamemode-unbound
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=0 ticks=8 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(no-ticks
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=0 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(ticks-differ-from-updates
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=5 match=1\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(match-inactive
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=8 match=0\n${_rhi}${_lifecycle}"
        FALSE)
    _spark_expect_arena_case(extra-field
        "${_ready}SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=8 match=1 x=1\n${_rhi}${_lifecycle}"
        FALSE)

    # Nonzero exit status fails even with a well-formed record.
    _spark_validate_fps_headless_arena(2 "${_ready}${_arena}${_rhi}${_lifecycle}" "" 72 4 _exit_ok _exit_reason)
    if(_exit_ok)
        message(FATAL_ERROR "Headless arena parser case 'nonzero-exit' unexpectedly passed")
    endif()
    # An empty or unparsable authored scene can never be matched.
    _spark_validate_fps_headless_arena(0 "${_ready}${_arena}${_rhi}${_lifecycle}" "" 0 0 _empty_ok _empty_reason)
    if(_empty_ok)
        message(FATAL_ERROR "Headless arena parser case 'empty-authored-scene' unexpectedly passed")
    endif()

    # The scene counter must agree with the loader rules on a small fixture.
    set(_fixture "${CMAKE_CURRENT_BINARY_DIR}/spark_fps_headless_arena_fixture.scene")
    file(WRITE "${_fixture}"
        "# comment\n[Scene]\nname=Fixture\n"
        "[Object]\ntype=plane\nname=Floor\nposition=0,0,0\n"
        "[Terrain]\nname=Ground\n"
        "[Camera]\nname=Main\nposition=0,2,-20\n"
        "[SpawnPoint]\nname=A\ntag=default\nposition=0,2,-10\n"
        "[SpawnPoint]\nname=B\ntag = default \npriority=-3\nposition=1,2,-10\n"
        "[SpawnPoint]\nname=C\ntag=default\npriority=high\nposition=2,2,-10\n"
        "[SpawnPoint]\nname=D\ntag=wave_spawn\nposition=3,2,-10\n"
        "[Enemy]\nname=NotANode\n"
        "[SpawnPoint]\nname=E\nposition=4,2,-10\ntag=default\n")
    _spark_count_authored_arena("${_fixture}" _fixture_objects _fixture_spawns)
    file(REMOVE "${_fixture}")
    if(NOT _fixture_objects EQUAL 8 OR NOT _fixture_spawns EQUAL 3)
        message(FATAL_ERROR
            "Authored arena counter disagrees with the loader: ${_fixture_objects} nodes / ${_fixture_spawns} spawns, "
            "expected 8 / 3")
    endif()

    message(STATUS "SparkGameFPS headless arena parser contract passed")
    return()
endif()

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_WORKING_DIRECTORY SPARK_ARENA_SCENE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkFPSHeadlessArena.cmake requires -D${_required}=<value>")
    endif()
endforeach()
foreach(_required_path SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_ARENA_SCENE)
    if(NOT EXISTS "${${_required_path}}")
        message(FATAL_ERROR "${_required_path} does not exist: ${${_required_path}}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Headless working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

_spark_count_authored_arena("${SPARK_ARENA_SCENE}" _expected_objects _expected_spawns)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=null"
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

_spark_validate_fps_headless_arena("${_result}" "${_stdout}" "${_stderr}" "${_expected_objects}"
    "${_expected_spawns}" _ok _reason)
if(NOT _ok)
    message(FATAL_ERROR
        "SparkGameFPS headless arena evidence failed (${CMAKE_HOST_SYSTEM_NAME} host): ${_reason}.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

message(STATUS
    "SparkGameFPS headless arena simulated ${_expected_objects} authored nodes and ${_expected_spawns} spawns on NullRHI")
