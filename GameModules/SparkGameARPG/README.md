# SparkGameARPG

SparkGameARPG is a compact systems-first action-RPG example. Loading the module now creates a level-one Barbarian, teaches a starter skill, enters dungeon floor one, and spawns a repeatable encounter. Defeating three enemies advances the dungeon and generates one loot drop per kill.

## Playable controls

- `Space`: basic attack
- `Q`: use the learned primary skill
- `R`: restart at floor one with full health and mana

The same loop is scriptable through `arpg_encounter`, `arpg_attack`, `arpg_cast`, and `arpg_restart`. The module intentionally uses its existing debug UI and engine integrations rather than shipping duplicate template-local art; monster, loot, skill, dungeon, combat, save, and AI state remain inspectable from the editor.

Skill learning and casting are validated against the authoritative hero ID, class, and level. If another gameplay system defeats the current encounter target, the encounter reconciles that death on its next update, awards progression once, and spawns the next target.

## Example boundary

This is a deterministic vertical slice, not a content-complete ARPG. Its purpose is to demonstrate a real cross-system loop with stable IDs and testable progression while leaving presentation, authored maps, and larger ability content to projects built from the template.
