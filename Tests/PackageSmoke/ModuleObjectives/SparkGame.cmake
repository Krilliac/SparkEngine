# MOD-300: packaged smoke of the SparkGame showcase. Included by Tests/PackageSmoke/RunInstalledModuleObjective.cmake
# (grammar documented there), which installs the runtime and samples components and runs the installed SparkEngine
# and SparkGame from the installed bin directory, with an empty environment on POSIX hosts.
#
# run: status in English, spawn one entity, switch to French, status again. The French status proves the installed
#   package carries its own Assets/Localization/SparkGame string tables (the showcase loads them relative to the
#   working directory; the source tree is never on the path), and the spawned count must rise by exactly one.

set(SPARK_OBJECTIVE_PHASES run)

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
