# MOD-370: the installed SparkGameRTS plays a deterministic skirmish to a Human victory. Included by
# Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# runA and runB are two separate processes. Each restarts the default skirmish with RTSScriptedCommander playing
# the Human side (rts_autoplay on); its orders are applied inside the fixed 32 Hz tick, so frame pacing cannot
# change the match. At 600 s of real loop time (the in-process scripted skirmish is capped at 15 simulated
# minutes and wins well inside it) each must report Victory, and runB's final tick, unit/building counts and full
# state hash must equal runA's: the match freezes its tick once decided, so the late read is stable.

set(SPARK_OBJECTIVE_PHASES runA runB)

foreach(_rts_run IN ITEMS runA runB)
    set(SPARK_OBJECTIVE_${_rts_run}_SCRIPT "1 rts_autoplay on
t600 rts_status
")
    set(SPARK_OBJECTIVE_${_rts_run}_SECONDS 605)
    set(SPARK_OBJECTIVE_${_rts_run}_EXPECT
        "rts_autoplay on|Autoplay on: default skirmish restarted"
        "rts_status|Match: Victory\n"
        "rts_status|State hash: [0-9a-f]+")
endforeach()
set(SPARK_OBJECTIVE_runB_SAME_AS "rts_status|runA")
