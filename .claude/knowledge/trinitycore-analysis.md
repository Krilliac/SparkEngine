# TrinityCore Architecture Analysis — What to Bring to SparkEngine

**Last updated:** 2026-03-18
**Type:** Decision
**Status:** Resolved (all 10 systems implemented and wired in)

## Description

Deep analysis of the TrinityCore MMO server emulator architecture, identifying patterns, systems, and design decisions that would strengthen SparkEngine's capabilities — particularly for MMO, RPG, and multiplayer game genres.

## Context

TrinityCore is a mature (~15+ years), production-tested C++ MMO framework emulating World of Warcraft servers. It handles thousands of concurrent players across seamless worlds with complex combat, AI, instancing, and persistence. SparkEngine already has HeroEngine-inspired networking (AreaServer/WorldServer), ECS (EnTT), AngelScript scripting, and behavior tree AI — but several TC patterns would significantly improve robustness and feature completeness.

## Analysis by System

---

### 1. SPELL/ABILITY SYSTEM → SparkEngine "AbilitySystem"

**What TC Has:**
- `SpellInfo` (static data), `Spell` (active cast instance), `Aura` (persistent effect), `AuraEffect` (individual effect within an aura)
- **Pipeline phases**: Preparation → Cast → Launch → Hit → Proc
- **Effect handler table**: Function pointer array indexed by effect type — O(1) dispatch to ~200+ effect handlers
- **Proc system**: Bitmask-driven triggered effects (spell school, hit type, family flags)
- **SpellScript / AuraScript**: Hook classes for per-spell custom behavior at each pipeline phase
- **Data-driven**: DBC/SQL tables define all spell behavior; C++ is the execution engine

**What SparkEngine Lacks:**
- No generalized ability/effect system (WeaponManager handles only FPS weapons)
- No aura/buff/debuff framework
- No proc/trigger chain system
- No data-driven ability definitions

**Recommendation — HIGH PRIORITY:**
Build an `AbilitySystem` with these TC-inspired patterns:
1. **AbilityDefinition** (static, data-driven) + **AbilityInstance** (active cast state) — mirrors SpellInfo/Spell
2. **EffectSystem** with typed effect handlers and a dispatch table — mirrors SpellEffects
3. **AuraSystem** for persistent effects (buffs, debuffs, DoTs, HoTs) with duration, stacking, periodic ticks
4. **ProcSystem** using bitmask event filters — when an entity deals/takes damage, check all active auras for proc triggers
5. **AbilityScript** hooks at each pipeline phase for custom behavior (AngelScript integration)

This is the single highest-value addition for RPG/MMO genres. The WeaponManager already has fire-rate/reload state machines — the AbilitySystem generalizes this pattern.

---

### 2. GRID/CELL SPATIAL MANAGEMENT → SparkEngine "SpatialGrid"

**What TC Has:**
- World divided into fixed-size **Grids** (533.33 units), each containing **Cells**
- `Map::VisitNearbyCellsOf()` with TypeContainerVisitor pattern — only visits cells within activation range
- Grid state machine: Active → Idle → Removal (with configurable unload delay)
- Objects tracked in move lists, relocated between cells at end of update
- Visibility distance determines which objects are sent to which players

**What SparkEngine Has:**
- SeamlessAreaManager for macro-level area streaming
- WorldOriginSystem for precision rebasing
- No fine-grained spatial partitioning within an area

**Recommendation — HIGH PRIORITY:**
Build a `SpatialGrid` system:
1. Divide each area into fixed-size cells (configurable, e.g. 64×64 units)
2. Track entity cell membership via ECS component (`CellComponent { cellX, cellY }`)
3. Provide `VisitNearby(position, radius, visitor)` for efficient spatial queries
4. Grid activation/deactivation tied to player proximity (only simulate active grids)
5. Feed into networking: only replicate entities in nearby cells to each player

This directly benefits: AI perception (replace brute-force range checks), networking (interest management), physics (broadphase), and rendering (culling).

---

### 3. MAP UPDATE THREADING → SparkEngine "ParallelAreaUpdate"

**What TC Has:**
- `MapUpdater` with producer-consumer thread pool
- Each `Map` update is a work item scheduled to the pool
- Maps updated in parallel; within a single map, update is single-threaded
- Main thread calls `wait()` to synchronize after all maps complete
- Cross-map access (e.g., group members on different maps) identified as data race risk

**What SparkEngine Has:**
- AreaServer design already isolates areas (each has own ECS, physics, AI)
- `ParallelSystemExecutor` exists for multi-threaded ECS system execution
- No explicit map-level parallelism in the update loop

**Recommendation — MEDIUM PRIORITY:**
SparkEngine's AreaServer architecture already provides better isolation than TC's MapUpdater (each area is a separate process/thread with its own ECS world). However, adopt TC's lesson:
1. Within a single AreaServer, use `ParallelSystemExecutor` for system-level parallelism
2. For cross-area operations (entity migration, chat, group data), use message queues — never direct memory access
3. Add per-area update time metrics (like TC's diff tracking) to detect overloaded areas

---

### 4. DATABASE & ASYNC PERSISTENCE → SparkEngine "AsyncPersistence"

**What TC Has:**
- `DatabaseWorkerPool<T>` — templated connection pool with sync/async modes
- `PreparedStatement` system — all queries are prepared at startup, referenced by enum ID
- Three databases: Auth, Characters, World (separation of concerns)
- Async queries via Boost.Asio — results delivered through futures
- Transaction batching — group related writes into atomic transactions
- `QueueSizeTracker` for monitoring database pressure
- Character save is periodic (configurable interval) + on-logout

**What SparkEngine Has:**
- SaveSystem exists but is file-based (not database-backed)
- No async query infrastructure
- No connection pooling
- No prepared statement pattern

**Recommendation — HIGH PRIORITY (for MMO/RPG):**
1. **AsyncQuerySystem**: Thread pool + work queue for database operations (SQLite for single-player, MySQL/PostgreSQL for MMO)
2. **PreparedStatementRegistry**: Define all queries at startup with typed enum IDs — prevents SQL injection, improves performance
3. **TransactionBatcher**: Group writes (e.g., character save = inventory + stats + position + quests) into atomic commits
4. **Periodic auto-save**: Configurable interval (TC uses 15 minutes default), plus save-on-disconnect
5. **Three-database pattern**: Separate auth/account data, character data, and world/static data

---

### 5. COMMAND SYSTEM WITH RBAC → SparkEngine "ConsoleRBAC"

**What TC Has:**
- `ChatHandler` parses commands, checks RBAC permissions
- `CommandTable` — static array mapping command strings → handler functions + permission IDs
- `RBAC.h` defines ~500+ granular permissions as enums
- Roles = collections of permissions; accounts have granted/denied permissions
- Deny overrides grant — explicit deny always wins
- Commands organized in `cs_*.cpp` script files (one per subsystem)

**What SparkEngine Has:**
- `SimpleConsole::RegisterCommand(name, handler, description, category, usage)` — functional but no permission model
- Console commands registered per-subsystem (already organized)
- No RBAC or permission levels

**Recommendation — MEDIUM PRIORITY:**
1. Add permission level to `RegisterCommand()` — even a simple integer level (0=player, 1=moderator, 2=admin, 3=developer)
2. For multiplayer: map connected sessions to permission levels
3. Store permissions in auth database (when async persistence is built)
4. Consolidate existing command registration as TC does — one `cs_*.cpp` per subsystem is already SparkEngine's pattern

---

### 6. SCRIPTING HOOK ARCHITECTURE → SparkEngine "ScriptHooks"

**What TC Has:**
- `ScriptMgr` singleton with 50+ hook categories (creature, spell, player, world, instance, etc.)
- `ScriptRegistry<TScript>` — template-based registry mapping script IDs to instances
- Factory pattern: `GetCreatureAI(creature)` returns polymorphic AI
- Hook macros: `FOREACH_SCRIPT(type)`, `GET_SCRIPT(type, id)` — iterate/dispatch
- **SmartAI**: Fully data-driven AI (database tables, not code) — `smart_scripts` table defines event→action→target chains
- **Script hot-reload**: `ScriptReloadMgr` watches for DLL changes

**What SparkEngine Has:**
- AngelScriptEngine with Start/Update/OnCollision hooks per entity
- BehaviorTree system (code-defined, not data-driven)
- ScriptHotReload for .as files
- No centralized hook dispatcher

**Recommendation — MEDIUM PRIORITY:**
1. **Expand script hooks**: Add hooks for combat events (OnDamageDealt, OnDamageTaken, OnKill, OnDeath), quest events, area transitions, spell casts
2. **Data-driven AI**: Build a "SmartAI" equivalent where behavior rules live in data files (JSON/YAML) rather than code — event/action/target triples
3. **Script type registry**: Formalize which script types exist (CreatureScript, SpellScript, AreaScript, InstanceScript) — each with its own hook interface
4. **Priority-based hook dispatch**: Multiple scripts can hook the same event; process in priority order with cancel capability

---

### 7. MOVEMENT SYSTEM WITH RECAST/DETOUR → SparkEngine "MovementSystem"

**What TC Has:**
- `MotionMaster` — per-unit movement controller managing a stack of `MovementGenerator`s
- Movement generators: Idle, Random, Waypoint, Chase, Follow, Flee, Charge, Point, Home, Flight, Spline
- **Movement slot priority**: Default < Motion < Active < Controlled — higher priority overrides lower
- Recast/Detour for navmesh pathfinding — industry-standard library
- `PathGenerator` creates navmesh queries with string-pulling for smooth paths
- MoveSpline for smooth interpolated movement along waypoints

**What SparkEngine Has:**
- Custom NavMesh with A* (triangle mesh, .snav binary format)
- AISystem handles movement via `currentPath` + `moveTarget`
- SteeringBehaviors for flocking/avoidance
- No movement generator stack or priority system
- No Recast/Detour integration

**Recommendation — MEDIUM PRIORITY:**
1. **MotionMaster pattern**: Implement a movement generator stack with priority slots — allows "flee" to override "patrol" which overrides "idle," then automatically resume lower-priority movement
2. **Consider Recast/Detour**: Industry-standard, battle-tested navmesh library. SparkEngine's custom NavMesh works, but Recast/Detour offers: automatic navmesh generation from geometry, dynamic obstacle support, tiled streaming, and is far more robust
3. **Movement splines**: Smooth interpolation along waypoints (TC's MoveSpline) — better visual quality than linear waypoint following
4. **Named movement types**: Enum-based generator types make debugging easier ("why is this NPC moving?" → "it's running ChaseMovementGenerator")

---

### 8. ENTITY UPDATE FIELDS / DIRTY TRACKING → SparkEngine "DirtyTracking"

**What TC Has:**
- `UpdateField<T, offset, index>` — template-based typed fields with automatic dirty flagging
- `SetUpdateFieldValue()` marks fields dirty → only dirty fields serialized in network updates
- `WriteCreate()` (full state for new observers) vs `WriteUpdate()` (delta for existing observers)
- Per-field visibility flags (public, private, party, owner-only)

**What SparkEngine Has:**
- `MarkPropertyDirty(entityId, propertyName)` in NetworkManager — string-based, manual
- Entity replication exists but without structured dirty tracking

**Recommendation — HIGH PRIORITY (for networking):**
1. **Typed update fields with automatic dirty bits**: When a replicated property changes, the dirty bit is set automatically (not manually)
2. **Delta compression**: Only send changed fields per network tick (TC's WriteUpdate pattern)
3. **Visibility masks**: Some fields only sent to owner (e.g., inventory), some to party, some to all — reduces bandwidth significantly
4. **Create vs Update packets**: Full state for entities entering view; deltas for ongoing updates

---

### 9. INSTANCE/DUNGEON SYSTEM → SparkEngine "InstanceManager"

**What TC Has:**
- `InstanceScript` — per-dungeon script managing boss encounters, doors, events
- Instance templates: Define which map is an instance, max players, reset timer
- Encounter state machine: NOT_STARTED → IN_PROGRESS → DONE / FAIL
- Instance-specific data persistence (boss kill states saved to DB)
- Lockout system: Prevent re-running cleared instances

**What SparkEngine Has:**
- AreaServer can serve as instance container (spawn a new AreaServer per instance)
- No encounter scripting framework
- No instance state persistence

**Recommendation — LOW-MEDIUM PRIORITY:**
1. **EncounterScript base class**: Start/InProgress/Success/Fail states, with hooks for boss phases
2. **Instance template data**: Define instance parameters in data files (max players, reset timer, encounter list)
3. **Encounter persistence**: Save/load boss kill states per player group
4. Leverage existing AreaServer — each instance is a dynamically spawned AreaServer with an EncounterScript

---

### 10. CONDITION SYSTEM → SparkEngine "ConditionSystem"

**What TC Has:**
- `ConditionMgr` — universal condition evaluation for all game systems
- Conditions: Is player class X? Has item Y? Quest Z complete? In area W? Level >= N?
- Used by: quest givers, spell targets, loot tables, gossip menus, SmartAI, event triggers
- Conditions loaded from database, evaluated at runtime
- Composable: AND/OR/NOT logic on condition groups

**What SparkEngine Has:**
- No centralized condition system
- Quest/dialogue systems likely have ad-hoc condition checks

**Recommendation — MEDIUM PRIORITY:**
1. **ConditionSystem**: Evaluate typed conditions (HasComponent, HasTag, InArea, HealthAbove, QuestComplete, etc.)
2. **Data-driven**: Conditions defined in data files, not hardcoded
3. **Composable**: AND/OR/NOT groups for complex requirements
4. **Universal**: Used by dialogue, quests, AI, loot, shops — single evaluation engine

---

### 11. LOGGING SYSTEM → SparkEngine Improvements

**What TC Has:**
- Multiple `Appender` types (Console, File, DB)
- Log channels with configurable severity per-channel
- `Logger` hierarchy with inheritance (e.g., `server.worldserver` inherits from `server`)
- Runtime log level changes via command

**What SparkEngine Has:**
- SimpleConsole with 7 severity levels, ring buffer, thread-safe
- Single output channel

**Recommendation — LOW PRIORITY:**
SparkEngine's logging is adequate. If needed later:
1. Add named log channels (Network, Physics, AI, Script, etc.) with per-channel severity
2. Add file appender alongside console output
3. Runtime severity control per channel via console command

---

## Priority Summary

| Priority | System | Effort | Impact |
|----------|--------|--------|--------|
| **HIGH** | AbilitySystem (spells/auras/procs) | Large | Enables RPG/MMO combat |
| **HIGH** | SpatialGrid (cell-based partitioning) | Medium | Benefits networking, AI, physics, rendering |
| **HIGH** | AsyncPersistence (database layer) | Large | Enables MMO persistence |
| **HIGH** | DirtyTracking (network update fields) | Medium | Massive bandwidth reduction |
| **MEDIUM** | ScriptHooks (expanded hook system) | Medium | Enables moddable gameplay |
| **MEDIUM** | ConsoleRBAC (permission system) | Small | Needed for multiplayer admin |
| **MEDIUM** | MovementSystem (generator stack + Recast) | Medium | Better NPC movement quality |
| **MEDIUM** | ConditionSystem (universal conditions) | Medium | Data-driven gameplay rules |
| **LOW-MED** | InstanceManager (dungeon instances) | Medium | Enables instanced content |
| **LOW** | Logging improvements | Small | Nice-to-have |

## Key Architectural Patterns to Adopt from TrinityCore

1. **Data-Driven Everything**: TC defines behavior in database tables, not C++. SmartAI, spell effects, loot tables, conditions — all data. SparkEngine should move toward JSON/YAML data definitions.

2. **Pipeline Phase Architecture**: TC's spell system processes casts through discrete phases with hooks at each point. This is cleaner than monolithic "do everything" functions.

3. **Function Pointer Dispatch Tables**: For effect handlers, movement generators, opcode handlers — O(1) lookup by type ID instead of switch statements.

4. **Typed Template Registries**: `ScriptRegistry<T>`, `DatabaseWorkerPool<T>` — type-safe singletons with compile-time guarantees.

5. **Visibility-Based Replication**: Only send data that the receiver needs and is allowed to see. Per-field visibility masks are more efficient than all-or-nothing entity replication.

6. **Priority Stacks**: Movement generators use priority slots. This pattern applies broadly — effects, AI behaviors, input handlers can all benefit from priority-based overrides with automatic fallback.

7. **Separation of Static vs Runtime Data**: SpellInfo (static template) vs Spell (active instance). This pattern recurs: WeaponDefinition/WeaponInstance, CreatureTemplate/Creature. SparkEngine already uses this for weapons and behavior trees.

## Notes

- TrinityCore is GPL-licensed. No code can be copied directly. Only architectural patterns and design ideas should be adopted.
- TC's object hierarchy (Object → WorldObject → Unit → Player) is traditional OOP. SparkEngine's ECS (EnTT) is architecturally superior for the same use cases — implement TC's patterns as components and systems, not class hierarchies.
- TC's single-threaded-per-map bottleneck is avoided by SparkEngine's AreaServer architecture, which isolates areas into separate processes.
- TC has ~15 years of battle-testing with thousands of concurrent users. Its patterns for bandwidth optimization, spatial management, and async persistence are proven at scale.
