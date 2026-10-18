# MOD-380: in the installed SparkGameRacing, the player on the production autopilot and the five AI drivers all
# finish valid laps of the default circuit on the Jolt-backed vehicles, under the host's shared fixed step.
# Included by Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# run: race_autopilot on restarts the race with the player driven by ComputePlayerAutopilotInput. At 400 s of
#   real loop time (the in-process races are budgeted 400 simulated seconds) every racer must be [FINISHED], none
#   [DNF], each on the final lap count, and the race must no longer be running.

set(SPARK_OBJECTIVE_PHASES run)

set(SPARK_OBJECTIVE_run_SCRIPT "1 race_autopilot on
t400 race_standings
t400 race_status
")
set(SPARK_OBJECTIVE_run_SECONDS 405)
set(SPARK_OBJECTIVE_run_EXPECT
    "race_autopilot on|Autopilot on: race restarted"
    "race_standings|Race Standings:"
    "race_status|Race State: Not Racing")
set(SPARK_OBJECTIVE_run_REJECT "race_standings|\\[DNF\\]")

# Six racers (the player and five AI drivers), every one finished with its lap counter on the race's total.
function(spark_objective_verify_run phase out_ok out_reason)
    spark_objective_block_count("${phase}" "race_standings" _count)
    spark_objective_block_output("${phase}" "race_standings" "${_count}" _standings)
    string(REGEX MATCHALL "  P[0-9]+ - [^\n]*" _racers "${_standings}")
    list(LENGTH _racers _racer_count)
    if(NOT _racer_count EQUAL 6)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "expected 6 racers in the standings, found ${_racer_count}:\n${_standings}" PARENT_SCOPE)
        return()
    endif()
    foreach(_racer IN LISTS _racers)
        if(NOT _racer MATCHES "\\| Lap ([0-9]+)/([0-9]+) .*\\[FINISHED\\]" OR NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_2 OR
           CMAKE_MATCH_2 LESS 1)
            set(${out_ok} FALSE PARENT_SCOPE)
            set(${out_reason} "racer did not finish every lap: ${_racer}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()
