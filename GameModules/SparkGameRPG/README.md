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

## Assets

RPGWorldSetup streams the Blender village and dungeon kit (`Assets/Models/RPG/Kit/`, authored by
`tools/blender/author_rpg_kit.py`; see `Art/Blender/SparkGameRPG/README.md`) with each area's manifest: the
village well, quest signpost and barrel in Oakhollow, signposts on the Forest and Swamp trails, and wall
sconces, treasure chests and barrels in the Shadow Crypt and Thornwall Castle. RPGEngineSystems registers the
five music cues in `Assets/Audio/RPG/Music/`. `asset-references.json` records every asset path the sources
name, with its sha256 and provenance rule, for `tools/check-module-asset-refs.py`.

## Build and test

Build the SparkGameRPG and SparkTests targets. The focused regression source is
Tests/TestGameModuleRPG.cpp. Tests/TestMOD350RPGQuestSliceReal.cpp drives the
quest chain (Shadow Wolves, Healing Herbs, The Dark Below) end to end through the
session API with the real RPGGameplayBridge quest policy installed; run it with
`ctest --test-dir build/linux-gcc-release -R RPGQuestSlice --output-on-failure`.

## NPC navigation

SparkGameRPGModule bakes one engine NavMesh per NPC area at load
(`RPGNPCSystem::BuildAreaNavigation`: a ground quad at y = 0 over the area's XZ bounds, built by
`Spark::AI::NavMeshBuilder`, which uses Recast when `ENABLE_RECAST` is on). When the world clock
moves an NPC into a new schedule entry, the NPC switches behavior at once and walks to the entry's
post at 3 m/s along a `NavMeshQuery::FindPath` route; a patrolling NPC walks to the waypoint it was
heading for. An NPC whose post is off the NavMesh or unreachable stays where it is. Routes are not
saved: after a load, the NPC plans a new route from its restored position. The module fails to load
if an NPC's area cannot be baked. Patrol legs still walk in straight lines between waypoints.
`ctest --test-dir build/linux-gcc-release -R RPGNPCNavigation --output-on-failure` runs the tests
(Tests/TestMOD350RPGNPCNavigationReal.cpp).

## Save and load

rpg_save and rpg_load go through RPGEngineSystems::SaveGame and LoadGame. The ECS
world is written to a SaveSystem slot (`<slot>.spark_save`, CRC-32 protected), and the
adventure is stored in the same file as the `SparkGameRPG.demo.v1` custom state. That
custom state is an `RPGDEMO 3` snapshot. It holds class, level, XP, health, mana, stats,
area, the current enemy, the pack, equipment and engine QuestSystem progress. It also holds
the NPC system's world clock and hour and, for every NPC, disposition, behavior, position
and patrol waypoint and wait timer.

SaveSystem validates the snapshot before it replaces the ECS world. A load is rejected,
and the world and adventure stay unchanged, if the snapshot is `RPGDEMO 1` or older or
a version newer than 3, is truncated or has trailing data, leaves out an NPC or names one
twice or names an unknown one, or holds a value out of range (disposition outside 0-100,
hour outside [0, 24), a waypoint the NPC's path does not have, a non-finite number). A
damaged or missing slot file is rejected in the same way.

`RPGDEMO 2` saves from the previous build still load (owner decision OD-03: read N and
N-1). Version 2 never stored NPCs, so the character, pack, equipment and quests load as
saved while the NPCs and world clock start from a new world's defaults
(`RPGNPCSystem::CaptureDefaultState`: 8:00, default dispositions, patrol at waypoint 0).
The next save writes `RPGDEMO 3`.

Tests/TestMOD350RPGQuestSliceReal.cpp also holds the RPGPersistence_* restart tests.
They save mid-quest, rebuild every RPG system and the QuestSystem, load the slot, finish
the quest, and restart once more to check that the reward was kept. A further test loads
an `RPGDEMO 2` slot and checks the migration to default NPC state. Run them with
`ctest --test-dir build/linux-gcc-release -R RPGPersistence --output-on-failure`.
There is no packaged or windows-shipping run of this flow yet.
