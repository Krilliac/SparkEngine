# SparkGameRPG

SparkGameRPG is a playable classic-RPG example built from SparkEngine's shared
quest, dialogue, save, AI, cinematic, weather, time-of-day, music, and ECS
services. The Oakhollow Adventure editor panel connects the module's character,
combat, inventory, NPC, and world-area systems into one live loop.

## Playable loop

1. Start as Rowan the Warrior, or choose another class with rpg_restart.
2. Talk to Oakhollow NPCs, inspect the quest journal, and travel to a connected area.
3. Fight deterministic area encounters, manage cooldowns, health, mana, and potions.
   When the class ability costs more mana than remains, rpg_attack falls back to a
   mana-free weapon strike.
4. Loot Moonpetal herbs, earn XP, complete objectives, and unlock the chained quests.
   Quest item rewards land in Rowan's pack before the quest is marked complete; a
   pack too full to hold them keeps the quest active so the reward is not lost.
5. Flee to Oakhollow when overwhelmed, rest at the inn, then continue toward the
   Shadow Crypt and Thornwall Castle.

The editor panel exposes travel, combat, healing, fleeing, resting, and nearby-NPC
actions. Equivalent console commands make the example usable in headless and
automated sessions:

    rpg_help
    rpg_play
    rpg_restart [warrior|mage|ranger|cleric|rogue|paladin]
    rpg_travel <area-id>
    rpg_attack
    rpg_flee
    rpg_rest
    rpg_talk <npc-id>
    rpg_use <item-id>
    rpg_accept <quest-id>

Discovery and engine-integration commands include rpg_areas, rpg_classes,
rpg_items, rpg_quests, rpg_npcs, rpg_save, rpg_load, rpg_weather, and rpg_time.

## Build and test

Build the SparkGameRPG and SparkTests targets. The focused regression source is
Tests/TestGameModuleRPG.cpp. Tests/TestMOD350RPGQuestSliceReal.cpp drives the
quest chain (Shadow Wolves, Healing Herbs, The Dark Below) end to end through the
session API with the real RPGGameplayBridge quest policy installed; run it with
`ctest --test-dir build/linux-gcc-release -R RPGQuestSlice --output-on-failure`.
