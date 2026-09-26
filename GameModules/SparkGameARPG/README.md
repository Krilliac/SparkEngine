# SparkGameARPG

SparkGameARPG is a compact systems-first action-RPG example. Loading the module now creates a level-one Barbarian, teaches a starter skill, enters dungeon floor one, and spawns a repeatable encounter. Defeating three enemies advances the dungeon and generates one loot drop per kill.

## Playable controls

- `Space`: basic attack
- `Q`: use the learned primary skill
- `R`: restart at floor one with full health and mana

The same loop is scriptable through `arpg_encounter`, `arpg_attack`, `arpg_cast`, and `arpg_restart`. Monster, loot, skill, dungeon, combat, save, and AI state remain inspectable from the editor through the module's debug UI.

## Crypt kit

When the module loads with a world, `ARPGDungeonSystem` dresses the crypt entry room with the Blender-authored Action RPG Dungeon kit in `Assets/Models/ARPG/Kit`: three destructible urns, a spike trap, a loot pile, and the portal gate to the next floor. The ModuleKits ARPG landmarks in `Assets/Models/ModuleKits/ARPG` frame the boss arena: two necrotic combat pillars, two summoner ritual braziers beside the portal gate, and the arcane loot chest next to the hoard. The props are set dressing only, with no colliders, triggers, or destructible components. Source, provenance, and preview are in `Art/Blender/SparkGameARPG/` and `Tools/model_pipeline/`, and `asset-references.json` records every asset path the module source names.

## World actors

`ARPGActorPresentation` gives the hero and every live monster a body in the ECS World: a `Transform`, a `MeshRenderer` using `Assets/Models/character.obj`, and a `HealthComponent` that mirrors the gameplay health each frame. The hero stands at the crypt entrance facing into the dungeon. Ordinary targets meet the hero in the aisle, and the boss stands between the pillars and is drawn larger. A killed monster's entity is destroyed with it, and a restart or restore replaces the actors. The gameplay systems stay authoritative, and the entities only mirror them. After `arpg_load` replaces the World's entities, the kit and the actors are rebuilt by name, so a load never leaves duplicates or stale entity IDs.

Skill learning and casting are validated against the authoritative hero ID, class, and level. Health and mana recover at deterministic, frame-rate-independent rates while the hero is alive, so the playable loop does not strand the starter hero after spending the initial mana pool. If another gameplay system defeats the current encounter target, the encounter reconciles that death on its next update, awards progression once, and spawns the next target.

## Save and load

`arpg_save [slot]` and `arpg_load [slot]` store the encounter as an `ARPGDEMO 4` snapshot in `SaveSystem` custom state (key `SparkGameARPG.demo.v1`), in the same atomic file as the ECS world. The snapshot carries the hero's stats and progression, the floor and kill counts, the current target in full (boss name, rank and affixes included), every learned skill with its remaining cooldown, and the carried loot. The hero carries up to 64 drops, and `R` keeps them. Restored items keep their IDs, and later drops are numbered after them. A load validates the whole snapshot before it changes anything: older `ARPGDEMO` versions, truncated data, loot that the generator could not have produced, and skills the hero's class or level cannot hold are all rejected.

The example also registers four engine-native abilities, four auras, and the Fire Mastery proc through `AbilitySystem`. Successful basic attacks and casts drive a real `AnimationStateMachine`; one-shot Attack and Cast states return to Idle through `CoroutineScheduler`, with a deterministic local-timer fallback for stripped/headless contexts. Use `arpg_abilities` or the ARPG Engine Integration debug panel to inspect those bridges live.

## Example boundary

This is a deterministic vertical slice, not a content-complete ARPG. Its purpose is to demonstrate a real cross-system loop with stable IDs and testable progression while leaving presentation, authored maps, and larger ability content to projects built from the template.
