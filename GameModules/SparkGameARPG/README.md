# SparkGameARPG

SparkGameARPG is a compact systems-first action-RPG example. Loading the module now creates a level-one Barbarian, teaches a starter skill, enters dungeon floor one, and spawns a repeatable encounter. Defeating three enemies advances the dungeon and generates one loot drop per kill.

## Playable controls

- `Space`: basic attack
- `Q`: use the learned primary skill
- `R`: restart at floor one with full health and mana

The same loop is scriptable through `arpg_encounter`, `arpg_attack`, `arpg_cast`, and `arpg_restart`. The module intentionally uses its existing debug UI and engine integrations rather than shipping duplicate template-local art; monster, loot, skill, dungeon, combat, save, and AI state remain inspectable from the editor.

Skill learning and casting are validated against the authoritative hero ID, class, and level. Health and mana recover at deterministic, frame-rate-independent rates while the hero is alive, so the playable loop does not strand the starter hero after spending the initial mana pool. If another gameplay system defeats the current encounter target, the encounter reconciles that death on its next update, awards progression once, and spawns the next target.

## Save and load

`arpg_save [slot]` and `arpg_load [slot]` store the encounter as an `ARPGDEMO 4` snapshot in `SaveSystem` custom state (key `SparkGameARPG.demo.v1`), in the same atomic file as the ECS world. The snapshot carries the hero's stats and progression, the floor and kill counts, the current target in full (boss name, rank and affixes included), every learned skill with its remaining cooldown, and the carried loot. The hero carries up to 64 drops, and `R` keeps them. Restored items keep their IDs, and later drops are numbered after them. A load validates the whole snapshot before it changes anything: older `ARPGDEMO` versions, truncated data, loot that the generator could not have produced, and skills the hero's class or level cannot hold are all rejected.

The example also registers four engine-native abilities, four auras, and the Fire Mastery proc through `AbilitySystem`. Successful basic attacks and casts drive a real `AnimationStateMachine`; one-shot Attack and Cast states return to Idle through `CoroutineScheduler`, with a deterministic local-timer fallback for stripped/headless contexts. Use `arpg_abilities` or the ARPG Engine Integration debug panel to inspect those bridges live.

## Example boundary

This is a deterministic vertical slice, not a content-complete ARPG. Its purpose is to demonstrate a real cross-system loop with stable IDs and testable progression while leaving presentation, authored maps, and larger ability content to projects built from the template.
