# MOD-330: the installed SparkGameARPG clears its dungeon under the automated player and the cleared run survives
# a process restart. Included by Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# writer: from t=0.05 s, twenty inputs per second alternate arpg_cast / arpg_attack -- the entry points the Q and
#   Space keys call, each printing ARPGDemoEncounter::GetStatusString -- for 75 s of real module frames (skill
#   cooldowns and monster updates run on real dt). Then, in one frame, arpg_encounter and arpg_save.
# reader: a fresh process shows a fresh run, then arpg_load and arpg_encounter in one frame must print the
#   writer's cleared status byte for byte.

set(SPARK_OBJECTIVE_PHASES writer reader)

set(_arpg_inputs 1500) # 75 s at 20 inputs per second
set(SPARK_OBJECTIVE_writer_SCRIPT "")
foreach(_input RANGE 1 ${_arpg_inputs})
    math(EXPR _hundredths "${_input} * 5")
    math(EXPR _seconds "${_hundredths} / 100")
    math(EXPR _fraction "${_hundredths} % 100")
    if(_fraction LESS 10)
        set(_fraction "0${_fraction}")
    endif()
    math(EXPR _parity "${_input} % 2")
    if(_parity EQUAL 1)
        string(APPEND SPARK_OBJECTIVE_writer_SCRIPT "t${_seconds}.${_fraction} arpg_cast\n")
    else()
        string(APPEND SPARK_OBJECTIVE_writer_SCRIPT "t${_seconds}.${_fraction} arpg_attack\n")
    endif()
endforeach()
string(APPEND SPARK_OBJECTIVE_writer_SCRIPT "t80 arpg_encounter\nt80 arpg_save mod330\n")
set(SPARK_OBJECTIVE_writer_SECONDS 85)
set(SPARK_OBJECTIVE_writer_EXPECT
    "arpg_encounter|Dungeon cleared: boss defeated on floor 5"
    "arpg_encounter|Floor: 5 \\| Kills: 13 \\("
    "arpg_save mod330|ARPG state saved to slot: mod330")

# Every boss-floor status line between the boss's arrival and the clear names the same [Boss] target with
# health that never rises: the packaged build fights one authoritative boss, it never re-rolls or heals it.
function(spark_objective_verify_writer phase out_ok out_reason)
    spark_objective_block_order("${phase}" _order)
    set(_boss "")
    set(_health "")
    set(_boss_blocks 0)
    foreach(_entry IN LISTS _order)
        if(NOT _entry MATCHES "^(arpg_attack|arpg_cast)#([0-9]+)$")
            continue()
        endif()
        spark_objective_block_output("${phase}" "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" _output)
        if(_output MATCHES "Dungeon cleared: boss defeated on floor 5")
            if(_boss STREQUAL "")
                set(${out_ok} FALSE PARENT_SCOPE)
                set(${out_reason} "the dungeon cleared before any status line showed a [Boss] target" PARENT_SCOPE)
                return()
            endif()
            set(${out_ok} TRUE PARENT_SCOPE)
            message(STATUS "ARPG boss '${_boss}' held its identity across ${_boss_blocks} status lines to the clear")
            return()
        endif()
        if(NOT _output MATCHES "Floor: 5 \\|")
            continue()
        endif()
        if(_output MATCHES "Target: ([^\n]+) \\[Boss\\] Lv[0-9]+ HP ([-+.0-9eE]+)/")
            set(_name "${CMAKE_MATCH_1}")
            set(_hp "${CMAKE_MATCH_2}")
            if(_boss STREQUAL "")
                set(_boss "${_name}")
            elseif(NOT _name STREQUAL _boss)
                set(${out_ok} FALSE PARENT_SCOPE)
                set(${out_reason} "boss changed from '${_boss}' to '${_name}' at ${_entry}" PARENT_SCOPE)
                return()
            elseif(_hp GREATER _health)
                set(${out_ok} FALSE PARENT_SCOPE)
                set(${out_reason} "boss '${_boss}' health rose from ${_health} to ${_hp} at ${_entry}" PARENT_SCOPE)
                return()
            endif()
            set(_health "${_hp}")
            math(EXPR _boss_blocks "${_boss_blocks} + 1")
        elseif(NOT _boss STREQUAL "")
            set(${out_ok} FALSE PARENT_SCOPE)
            set(${out_reason} "a non-boss target replaced boss '${_boss}' before the clear at ${_entry}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_ok} FALSE PARENT_SCOPE)
    set(${out_reason} "no arpg_attack/arpg_cast status line reported the cleared dungeon" PARENT_SCOPE)
endfunction()

set(SPARK_OBJECTIVE_reader_SCRIPT "1 arpg_encounter\n2 arpg_load mod330\n2 arpg_encounter\n")
set(SPARK_OBJECTIVE_reader_SECONDS 5)
set(SPARK_OBJECTIVE_reader_EXPECT
    "arpg_encounter#1|Floor: 1 \\| Kills: 0 "
    "arpg_load mod330|ARPG state loaded from slot: mod330")
set(SPARK_OBJECTIVE_reader_REJECT "arpg_encounter#1|Dungeon cleared")
set(SPARK_OBJECTIVE_reader_SAME_AS "arpg_encounter#2|writer|arpg_encounter")
