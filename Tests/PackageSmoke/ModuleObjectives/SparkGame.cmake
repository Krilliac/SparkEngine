# MOD-300: packaged smoke of the SparkGame showcase. Included by Tests/PackageSmoke/RunInstalledModuleObjective.cmake
# (grammar documented there), which installs the runtime and samples components and runs the installed SparkEngine
# and SparkGame from the installed bin directory, with an empty environment on POSIX hosts.
#
# run: status in English, spawn one entity, switch to French, status again. The French status proves the installed
#   package carries its own Assets/Localization/SparkGame string tables (the showcase loads them relative to the
#   working directory; the source tree is never on the path), and the spawned count must rise by exactly one.
# runA, runB: two more fresh processes each print showcase_outcome at 7 s of loop time. The coroutine sequence
#   (spawn, 3 s, damage, 2 s, heal) must be complete with its one damage event, and all four exhibit props must
#   name meshes that exist under the installed bin directory. runB's outcome must equal runA's in every field but
#   the time-of-day hour, which advances with wall-clock-paced frames.

set(SPARK_OBJECTIVE_PHASES run runA runB)

set(SPARK_OBJECTIVE_run_SCRIPT "1 showcase_status
2 showcase_spawn PackagedProbe
3 showcase_language fr
4 showcase_status
")
set(SPARK_OBJECTIVE_run_SECONDS 5)
set(SPARK_OBJECTIVE_run_EXPECT
    "showcase_status#1|=== Gameplay Showcase Status ===\nLanguage: en\n"
    "showcase_language fr|Language set to fr"
    "showcase_status#2|Langue: fr\n")

# Spawned entities before, the spawn result, and the French count after must agree: exactly one more entity.
function(spark_objective_verify_run phase out_ok out_reason)
    spark_objective_block_output("${phase}" "showcase_status" 1 _before)
    spark_objective_block_output("${phase}" "showcase_spawn PackagedProbe" 1 _spawn)
    spark_objective_block_output("${phase}" "showcase_status" 2 _after)
    if(NOT _before MATCHES "Spawned entities: ([0-9]+)\n")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "the first status has no spawned-entity count:\n${_before}" PARENT_SCOPE)
        return()
    endif()
    math(EXPR _expected "${CMAKE_MATCH_1} + 1")
    if(NOT _spawn MATCHES "Spawned 'PackagedProbe' \\(id=[0-9]+, total=${_expected}\\)")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "the spawn did not report total=${_expected}:\n${_spawn}" PARENT_SCOPE)
        return()
    endif()
    if(NOT _after MATCHES "Entités créées: ${_expected}\n")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "the French status does not count ${_expected} entities:\n${_after}" PARENT_SCOPE)
        return()
    endif()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

foreach(_showcase_run IN ITEMS runA runB)
    set(SPARK_OBJECTIVE_${_showcase_run}_SCRIPT "t7 showcase_outcome
")
    set(SPARK_OBJECTIVE_${_showcase_run}_SECONDS 9)
    set(SPARK_OBJECTIVE_${_showcase_run}_EXPECT
        "showcase_outcome|SPARK_SHOWCASE_OUTCOME stage=complete target_hp=100 damage_events=1 kill_events=0 "
        "showcase_outcome| exhibit=4/4 spawned=4(\n|$)")
endforeach()

# The outcome line of a phase's showcase_outcome block with its hour field removed.
function(_spark_showcase_outcome_without_hour phase out_line)
    spark_objective_block_output("${phase}" "showcase_outcome" 1 _block)
    if(NOT _block MATCHES "(SPARK_SHOWCASE_OUTCOME [^\n]*)")
        set(${out_line} "" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE " hour=[^ ]+" "" _line "${CMAKE_MATCH_1}")
    set(${out_line} "${_line}" PARENT_SCOPE)
endfunction()

function(spark_objective_verify_runB phase out_ok out_reason)
    _spark_showcase_outcome_without_hour(runA _first)
    _spark_showcase_outcome_without_hour("${phase}" _second)
    if(_first STREQUAL "" OR NOT _first STREQUAL _second)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "runB's outcome differs from runA's (hour excluded):\n${_first}\n${_second}" PARENT_SCOPE)
        return()
    endif()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()
