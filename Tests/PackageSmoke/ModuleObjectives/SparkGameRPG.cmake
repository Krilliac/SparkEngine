# MOD-350: the installed SparkGameRPG finishes the wolf hunt under the automated player, and the completed quest,
# its reward and the hero's state survive a process restart. Included by
# Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# writer: rpg_autoplay hunts Shadow Wolves through the session actions (rest, travel, attack, flee) with real
#   ability cooldowns; at 240 s the autopilot must report the quest completed, the journal must hold it as
#   completed, and the slot is saved.
# reader: a fresh process first shows the quest still active (nothing leaked into it), then rpg_load must bring
#   back the writer's journal and the writer's hero status (level, XP from the kills and the quest reward, health,
#   mana, area, gold and carried weight, which includes the potion reward) in the same frame.

set(SPARK_OBJECTIVE_PHASES writer reader)

set(SPARK_OBJECTIVE_writer_SCRIPT "1 rpg_autoplay 1 Shadow Wolf
t240 rpg_autoplay
t240 rpg_quests
t240 rpg_play
t240 rpg_save packaged
")
set(SPARK_OBJECTIVE_writer_SECONDS 245)
set(SPARK_OBJECTIVE_writer_EXPECT
    "rpg_autoplay 1 Shadow Wolf|Autoplay started for quest 1, hunting Shadow Wolf"
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
