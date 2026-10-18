# MOD-350: the installed SparkGameRPG finishes the wolf hunt under the automated player, and the completed quest,
# its reward and the hero's state survive a process restart. Included by
# Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# The patrolling guard (NPC 3, Village) is sampled with rpg_npc_route while the autopilot hunts. It starts on its
#   first waypoint (-50, 0, 100) with waypointWaitTimer 0 (NPCData default; the waypoint's 3 s waitTime only applies
#   when it arrives back there), so the first RPGNPCSystem::Update plans the 100 m NavMesh leg to (50, 0, 100) and
#   it walks at WALK_SPEED 3 m/s: about 30 m along by 10 s and 60 m by 20 s, both inside the ~33 s leg. The samples
#   at 10 s and 20 s must therefore show a route in progress and a position that advanced along it (~30 m apart;
#   the verifier below demands at least 10 m, which absorbs any frames the system ran before the script's clock).
#
# writer: rpg_autoplay hunts Shadow Wolves through the session actions (rest, travel, attack, flee) with real
#   ability cooldowns; at 240 s the autopilot must report the quest completed, the journal must hold it as
#   completed, and the slot is saved.
# reader: a fresh process first shows the quest still active (nothing leaked into it), then rpg_load must bring
#   back the writer's journal and the writer's hero status (level, XP from the kills and the quest reward, health,
#   mana, area, gold and carried weight, which includes the potion reward) in the same frame.

set(SPARK_OBJECTIVE_PHASES writer reader)

set(SPARK_OBJECTIVE_writer_SCRIPT "1 rpg_autoplay 1 Shadow Wolf
t10 rpg_npc_route 3
t20 rpg_npc_route 3
t240 rpg_autoplay
t240 rpg_quests
t240 rpg_play
t240 rpg_save packaged
")
set(SPARK_OBJECTIVE_writer_SECONDS 245)
set(SPARK_OBJECTIVE_writer_EXPECT
    "rpg_autoplay 1 Shadow Wolf|Autoplay started for quest 1, hunting Shadow Wolf"
    "rpg_npc_route 3#1|behavior=Patrol pos=\\(.*\\) route=[0-9]+/[1-9][0-9]*"
    "rpg_npc_route 3#2|behavior=Patrol pos=\\(.*\\) route=[0-9]+/[1-9][0-9]*"
    "rpg_autoplay|Autoplay quest 1: completed \\(trips [0-9]+, kills 5\\)"
    "rpg_quests|Entity [0-9]+: 0 active, 1 completed"
    "rpg_play|Active quests: 0\n"
    "rpg_save packaged|Saved RPG world")
set(SPARK_OBJECTIVE_writer_REJECT "rpg_play|HP 0/")

set(SPARK_OBJECTIVE_reader_SCRIPT "1 rpg_quests
2 rpg_load packaged
2 rpg_quests
2 rpg_play
")
set(SPARK_OBJECTIVE_reader_SECONDS 5)
set(SPARK_OBJECTIVE_reader_EXPECT
    "rpg_quests#1|Entity [0-9]+: 1 active, 0 completed"
    "rpg_load packaged|Loaded RPG world")
set(SPARK_OBJECTIVE_reader_REJECT "rpg_quests#1|0 active, 1 completed")
set(SPARK_OBJECTIVE_reader_SAME_AS "rpg_quests#2|writer|rpg_quests")

# The guard must have walked between the two samples: the NavMesh route advanced the position by at least 10 m.
function(spark_objective_verify_writer phase out_ok out_reason)
    set(_pattern "pos=\\((-?[0-9]+\\.[0-9]+), (-?[0-9]+\\.[0-9]+), (-?[0-9]+\\.[0-9]+)\\)")
    spark_objective_block_output("${phase}" "rpg_npc_route 3" 1 _first)
    spark_objective_block_output("${phase}" "rpg_npc_route 3" 2 _second)
    if(NOT _first MATCHES "${_pattern}")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "first rpg_npc_route sample has no position:\n${_first}" PARENT_SCOPE)
        return()
    endif()
    set(_x1 "${CMAKE_MATCH_1}")
    set(_z1 "${CMAKE_MATCH_3}")
    if(NOT _second MATCHES "${_pattern}")
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "second rpg_npc_route sample has no position:\n${_second}" PARENT_SCOPE)
        return()
    endif()
    set(_x2 "${CMAKE_MATCH_1}")
    set(_z2 "${CMAKE_MATCH_3}")
    # CMake math is integer-only: compare the whole-metre parts of the two samples.
    string(REGEX REPLACE "\\.[0-9]+$" "" _x1 "${_x1}")
    string(REGEX REPLACE "\\.[0-9]+$" "" _x2 "${_x2}")
    string(REGEX REPLACE "\\.[0-9]+$" "" _z1 "${_z1}")
    string(REGEX REPLACE "\\.[0-9]+$" "" _z2 "${_z2}")
    math(EXPR _dx "${_x2} - ${_x1}")
    math(EXPR _dz "${_z2} - ${_z1}")
    math(EXPR _moved "${_dx} * ${_dx} + ${_dz} * ${_dz}")
    if(_moved LESS 100)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "the guard did not advance along its route between samples (squared distance ${_moved}):\n${_first}\n---\n${_second}" PARENT_SCOPE)
        return()
    endif()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

# The restored hero status must equal the writer's line for line, except "Last action:", which records the
# writer's final attack and is not part of the save.
function(spark_objective_verify_reader phase out_ok out_reason)
    spark_objective_block_count("writer" "rpg_play" _writer_count)
    spark_objective_block_output("writer" "rpg_play" "${_writer_count}" _writer)
    spark_objective_block_count("${phase}" "rpg_play" _reader_count)
    spark_objective_block_output("${phase}" "rpg_play" "${_reader_count}" _reader)
    string(REGEX REPLACE "Last action:[^\n]*" "" _writer "${_writer}")
    string(REGEX REPLACE "Last action:[^\n]*" "" _reader "${_reader}")
    if(_writer STREQUAL "" OR NOT _writer STREQUAL _reader)
        set(${out_ok} FALSE PARENT_SCOPE)
        set(${out_reason} "restored hero status differs from the writer's:\n${_reader}\n---\n${_writer}" PARENT_SCOPE)
        return()
    endif()
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()
