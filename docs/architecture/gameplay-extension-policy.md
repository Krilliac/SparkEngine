# Gameplay Extension Policy

## Objective

Keep core gameplay systems (Quest, Dialogue, Inventory, Ability) deterministic and reusable while allowing module-specific behavior through explicit extension points.

## Duplicate Inventory and Decisions

| Pattern | Engine Path | Module Path(s) | Decision | Rationale |
|---|---|---|---|---|
| Quest tracking + objectives | `SparkEngine/Source/Engine/Gameplay/QuestSystem.*` | `GameModules/SparkGameRPG/Source/Quest/RPGQuestSystem.*`, `GameModules/SparkGameFPS/Source/Game/QuestSystem.h` | **Migrate to engine extension point** | Shared lifecycle/progression belongs in engine. Genre-specific gating/reward side effects are policy hooks. |
| Branching dialogue | `SparkEngine/Source/Engine/Dialogue/DialogueSystem.*` | `GameModules/SparkGameRPG/Source/Dialogue/RPGDialogueSystem.*` | **Migrate to engine extension point** | Branching + choice evaluation is generic. RPG specialization is condition/action hooks. |
| Inventory foundations | `SparkEngine/Source/Engine/Gameplay/InventorySystem.*` | `GameModules/SparkGameRPG/Source/Inventory/RPGInventorySystem.*`, `GameModules/SparkGameMMO/Source/Inventory/MMOInventorySystem.*` | **Keep module overrides** | Weight, durability, and shard/mail semantics are genre-specific enough to warrant module ownership. |
| Loot generation | _No engine equivalent today_ | `GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.*`, `GameModules/SparkGameFPS/Source/Game/LootSystem.*` | **Keep module overrides** | Loot loops are highly game-specific. Revisit only if a generic drop-table runtime is introduced. |
| Skill trees / ARPG skill model | _No engine equivalent today_ (engine has ability pipeline) | `GameModules/SparkGameARPG/Source/Skill/ARPGSkillSystem.*` | **Keep module override** | ARPG tree progression is distinct from engine's combat-cast pipeline. |
| Legacy RPG quest/dialogue implementations | Engine has equivalents now | `GameModules/SparkGameRPG/Source/Quest/RPGQuestSystem.*`, `GameModules/SparkGameRPG/Source/Dialogue/RPGDialogueSystem.*` | **Deprecate one path** (module impls) | Bridge module now consumes engine systems; old module-local implementations should be retired after migration window. |

## Extension Contract Rules

1. **Default rule:** extend engine gameplay systems; do not fork them in modules.
2. **Allowed fork:** only when mechanics are fundamentally domain-unique (e.g., ARPG affix generation, MMO mail-bank economy inventory).
3. **If you fork:**
   - document why the engine abstraction cannot represent the behavior,
   - define owner subsystem and test surface,
   - define exit criteria for converging back to engine extension points.
4. **If you extend:** use one of:
   - policy/strategy interfaces,
   - data-driven action/condition hooks,
   - config structs consumed by core systems.

## Current Representative Migration (RPG)

RPG now integrates with engine core systems instead of maintaining duplicated quest/dialogue runtimes:

- `QuestSystem` now supports a pluggable `IQuestPolicy` strategy.
- `DialogueSystem` now supports data-driven event action handlers.
- `RPGGameplayBridge` installs RPG quest policy + registers RPG dialogue hooks/tree data into engine systems.

This keeps core progression logic in engine and module-specific semantics in extension hooks.
