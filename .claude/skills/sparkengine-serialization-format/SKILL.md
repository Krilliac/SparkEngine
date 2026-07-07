---
name: sparkengine-serialization-format
description: The SparkEngine save-game and world-persistence formats — the SaveSystem `.spark_save` binary layout (magic, versioning, per-component string properties) and the AsyncDatabase persistence layer (async future/callback/sync API, shared-connection threading, file-backed key-value fallback). TRIGGER when the user says "add a field to the save file", "why isn't my component saved", "save/load format", "spark_save", "SaveSystem", "quicksave/autosave", "AsyncDatabasePool", "async query", "ProcessCallbacks", "prepared statement", "MMO persistence", "save version / migration", or "database won't persist". DO NOT TRIGGER for scene/entity file serialization or the reflection macros (see sparkengine-reflection-conventions), for AngelScript hot-reload state (see sparkengine-hot-reload-seam), or for network replication wire format.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine serialization: save-games and persistence

This skill covers **two distinct persistence subsystems**:

1. **SaveSystem** — full game-state save/load to `.spark_save` files
   (`SparkEngine/Source/Engine/SaveSystem/`).
2. **AsyncDatabase** — a thread-pooled key-value persistence layer
   (`SparkEngine/Source/Engine/Persistence/`), used by e.g. the MMO module for
   character/inventory storage.

## Scope — what this is NOT

- **Scene/entity `.scene` files and the `SPARK_REFLECT_*` macros** are a
  different serializer entirely — see **`sparkengine-reflection-conventions`**
  and `SceneManager/ReflectedSceneSerializer.cpp`. Do not conflate them.
  (SaveSystem *reuses* the reflection registry to auto-generate serializers for
  reflected components — that bridge is described below — but the on-disk
  format, versioning, and API are its own.)
- Network replication wire format is separate (the `replicated` field attribute
  lives in the reflection skill).

---

## Part 1 — SaveSystem (`.spark_save`)

### What it does

`SaveSystem` (singleton, `SaveSystem::GetInstance()`) serializes an ECS `World`
(entities + components + a free-form key/value `customState`) to a **custom,
uncompressed binary file** per save slot. It is initialized at engine startup
(`SparkEngineWindows.cpp` calls `SaveSystem::GetInstance().Initialize("Saves")`
and registers it on `EngineContext` via `SetSaveSystem`). Debug-console commands
`save_list` and `save_info <slot>` are wired in `EngineConsoleCommands.cpp`.

Public API (all return `bool` success unless noted; **not thread-safe — call
from the main thread**):

| Method | Purpose |
| --- | --- |
| `Initialize(dir = "Saves")` | Create save dir, register all built-in serializers. Call once. |
| `Save(slot, world, meta)` | Serialize `world` + `meta`, write `<dir>/<slot>.spark_save`. |
| `Load(slot, world)` | Parse file, **clear `world`**, rebuild entities. |
| `QuickSave/QuickLoad(world[, meta])` | Uses reserved slot `"__quicksave"`. |
| `AutoSave(world, meta)` | Rotates slots `"__autosave_0".."__autosave_{N-1}"` (N = `SetMaxAutoSaves`, default 3). |
| `SaveExists / DeleteSave / GetSaveSlots / GetSaveMetadata` | Slot management + metadata-only reads for UI. |
| `SerializeWorld(world, meta)` → `SaveData` | In-memory snapshot, no disk I/O (for undo/checkpoint). |
| `DeserializeWorld(data, world)` → bool | Restore from an in-memory `SaveData` (clears `world` first). |

**Slot-name rules** (`IsValidSlotName`): non-empty, ≤ 64 chars, only
`[A-Za-z0-9_-]`. Anything else is rejected (returns false / empty path). Do not
use `__quicksave` or `__autosave_*` for user-facing slots.

### On-disk binary layout (format version 1)

Verified against `WriteToFile` / `ReadFromFile` in `SaveSystem.cpp`. All
integers are written **raw via `reinterpret_cast` in host byte order** (little-
endian on all supported x86/x64/ARM64 targets) — saves are **not byte-swapped
and therefore not portable across opposite-endian machines**.

```
"SPRK"                     4 bytes  magic (rejected on load if mismatched)
uint32  version            = data.metadata.version (currently 1)
uint32  metaSize
bytes[metaSize]  metadata text block  (newline-delimited, see below)
uint32  entityCount        (load caps at 1,000,000)
  repeat entityCount times:
    uint16  nameLen  + name bytes          (load caps nameLen at 4096)
    uint16  compCount
      repeat compCount times:
        uint16  typeLen + typeName bytes    (load caps typeLen at 4096)
        uint16  propCount
          repeat propCount times:
            uint16 keyLen + key bytes
            uint16 valLen + value bytes
uint32  customStateCount   (optional/trailing; load caps at 100,000)
  repeat: uint16 keyLen+key, uint16 valLen+value
```

The **metadata text block** is newline/whitespace-delimited, read back in this
exact order (first 3 via `getline`, rest via `operator>>`):
`saveName`, `sceneName`, `playerClass`, `timestamp`, `playTime`,
`playerHealth`, `playerArmor`, `playerPosition.x y z`, `playerKills`,
`playerDeaths`.

**Writer safety rejections** (a save is *refused*, not silently corrupted, when):
- any of the first three metadata fields contains an embedded `\n`/`\r` (would
  desync the `getline` parse on load);
- any string exceeds `uint16` max (65535) — component/property counts too.

**Atomicity**: writes go to `<path>.tmp` then `std::filesystem::rename` over the
target, so a crash mid-write cannot corrupt an existing save.

### Versioning and migration

- Current version constant: `kCurrentSaveVersion = 1` (top of `SaveSystem.cpp`).
- On load: a file with `version > kCurrentSaveVersion` is **rejected** (forward-
  incompatible). A file with `version < kCurrentSaveVersion` logs a migration
  message and falls through the **migration hook** in `ReadFromFile`.
- The migration hook currently has **no actual upgrade branches** — version 1 is
  the baseline. When you change the layout, bump `kCurrentSaveVersion` and add a
  `if (version < N)` branch that fixes up the parsed fields.

### How components get saved — the two-step rule (important gotcha)

A component type participates in save/load only if **both** hold:

1. **A serializer is registered** for its type-name string, via one of:
   - a hand-written pair in `RegisterCoreSerializers` /
     `RegisterPhysicsSerializers` / `RegisterGameplaySerializers`
     (`ComponentSerializerRegistry::Register(name, serializeFn, deserializeFn)`),
     where each field is encoded as a **string property** (e.g. Transform writes
     `px/py/pz`, `rx/ry/rz`, `sx/sy/sz` via `std::to_string`); **or**
   - **auto-generated** by `RegisterReflectedSerializers()` — for every type in
     `ComponentFactory::GetRegisteredNames()` that has reflected fields and no
     hand-written serializer, it builds lambdas using `GetFieldAsString` /
     `SetFieldFromString` keyed by the C++ member name. (This is the reflection
     bridge; see `sparkengine-reflection-conventions` for how to reflect a type.)
   All of the above run from `RegisterBuiltins()`, called by `Initialize()`.

2. **The type is walked at save time.** `SaveSystem::SerializeWorld` iterates a
   **hard-coded list** of `TrySerialize<T>(...)` calls (Transform, MeshRenderer,
   HealthComponent, LightComponent, AudioSourceComponent, Camera, Script,
   RigidBodyComponent, ColliderComponent, ParticleEmitterComponent,
   AnimationController, NetworkIdentity, TagComponent, ActiveComponent — plus
   `NameComponent` for the entity name).

> **Consequence:** registering a serializer (even via reflection) is **not
> enough**. If your new component type is not in the `SerializeWorld`
> `TrySerialize<>` list, it is **silently never written to the save**. To add a
> savable component you must add both the serializer *and* a
> `TrySerialize<YourType>(world, entity, serializerRegistry, "YourType", se.components);`
> line in `SaveSystem::SerializeWorld`.

Load is symmetric: `DeserializeWorld` reads each component's `typeName` and
dispatches to the registered deserializer (unknown type-names are skipped).
Deserializers use `SafeGetFloat/SafeGetUint32/SafeGetString` helpers that
default missing/oversized values rather than throwing.

### `customState` is effectively unused via the public API (observation)

The format reserves a trailing `customState` key/value block, but
`SerializeWorld` **never populates it**, and `WriteToFile` is private. So through
the public `Save()` path `customState` is always empty on disk. To use it you
would need to construct a `SaveData` and write it yourself (no public writer
exists today). Treat the `SaveData::customState` field as a reserved,
not-yet-wired data slot, not a working feature. (`open`)

### Stale in-code documentation to ignore (verified mismatches)

These doc-comments contradict the actual code — trust the code:

| Says | Reality |
| --- | --- |
| `SaveSystem.cpp` file header: "JSON serialization"; `SaveSystemTypes.h`: "JSON representation on disk", `SerializedComponent` shows a JSON example | Format is the **custom binary** layout above. No JSON is written. |
| `SaveMetadata` doxygen: "compressed save file", "decompressing" | Files are **uncompressed**. |
| `SaveSystem.h`: console commands `save list` / `save info` | Actual command names are `save_list` / `save_info` (underscores). |

---

## Part 2 — AsyncDatabase (`Spark::Persistence`)

### What it is (and what the backend really is)

`AsyncDatabasePool` is a thread-pool persistence layer (TrinityCore-inspired:
`DatabaseWorkerPool`→pool, `PreparedStatement`→prepared-query-by-ID, results via
`std::future` or callback). The header comment says "Uses SQLite by default" —
but **the only `IDatabaseConnection` implementation is `SQLiteConnection`, which
is a file-backed key-value store, not real SQLite / not real SQL.** It persists
as tab-separated `key\tvalue\n` lines and understands only four command verbs via
`ExecuteRaw`:

| Command (case-insensitive) | Effect |
| --- | --- |
| `SET <key> <value…>` | Store value (rest of line, verbatim incl. newlines). Flushes to disk immediately unless in a transaction. |
| `GET <key>` | Returns 1 row, 1 column (the value) if present. |
| `DELETE <key>` | Remove key. Immediate flush if it removed something. |
| `KEYS [prefix]` | Return all keys (optionally prefix-filtered), one row each. |

Any other verb → `success = false`, `errorMessage = "Unsupported command…"`.

### Prepared statements and parameter substitution

- Register SQL by ID: `PrepareStatement(id, sql)` (may be called before or after
  `Open`; call before issuing queries).
- Parameters are referenced in the SQL string as **`?N` — zero-indexed**
  (`?0`, `?1`, …), substituted left-to-right in a single pass by
  `SQLiteConnection::Execute`. String params are single-quote-escaped (`'`→`''`).
  A `?N` with no matching bound param is emitted **verbatim**. Multi-digit
  indices work (`?10` is distinct from `?1` — this is a fixed regression, covered
  by `TestAsyncDatabaseRegressions.cpp`).
- Bind params by index with `PreparedStatementData::SetInt/SetDouble/SetString/
  SetNull`, or pass a `std::vector<PreparedStatementParam>` directly to the query
  calls.

### The three ways to run a query and get results

| Call | Returns result via | Threading |
| --- | --- | --- |
| `AsyncQuery(id, params)` | `std::future<QueryResult>` — call `.get()` (blocks) or poll | Runs on a worker thread. A query that throws sets the future's exception (rethrown on `.get()`). |
| `AsyncQueryWithCallback(id, params, cb)` | `cb(QueryResult)` dispatched **on the main thread** | You **must call `ProcessCallbacks()` once per frame** on the main loop or callbacks never fire. On exception, `cb` gets `success=false, errorMessage="Query threw exception"`. |
| `SyncQuery(id, params)` | `QueryResult` directly | Blocks the calling thread. Returns a failure result if the pool is closed. |
| `AsyncTransaction(tx)` | `std::future<QueryResult>` | Atomic batch (all-or-nothing): begins a transaction, runs each query, commits on success / rolls back on first failure. |

`QueryResult` fields: `success`, `errorMessage`, `rows` (`vector<QueryRow>`),
`affectedRows`, `lastInsertId`. `QueryRow::GetInt/GetDouble/GetString/IsNull(col)`
are bounds-checked and log + return a default on type/range mismatch (they do
**not** throw, despite the header doc-comment claiming `std::bad_variant_access`).

### Threading model — one shared connection, not one-per-worker

The header says "Each worker thread owns a dedicated database connection." **The
implementation deliberately does the opposite:** `Open()` creates a *single*
`m_syncConnection` and leaves the per-worker `m_connections` vector empty; every
query — async worker *and* `SyncQuery` — executes through that one connection
under `m_syncMutex`. This is intentional (in-code comment explains it): the
file-backed store keeps data in memory, so independent per-worker connections
would each hold a divergent copy and the last flush on `Close` would clobber the
others. **Net effect: all DB operations are fully serialized;** the worker pool
exists to service the async future/callback API, not to parallelize DB access.

- `AsyncDatabasePool` is **non-copyable and non-movable**.
- Lifecycle: `Open(connStr, poolSize=2)` → issue queries → `Close()` (joins
  workers, flushes, closes). Destructor calls `Close()` if still open.
- Transactions in the fallback store: `BeginTransaction` snapshots the map;
  `Commit` flushes; `Rollback` restores the snapshot. Non-transactional
  `SET`/`DELETE` flush immediately.

### Primary real-world consumer

`GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp` owns an
`AsyncDatabasePool`, prepares ~30 statements (characters, inventory, currency,
reputation, achievements, crafting, guilds, lockouts, boss kills), and calls
`m_db->ProcessCallbacks()` each tick. Use it as the reference integration
example.

---

## Open items and candidates (do not assert as fixed)

- **`%N` vs `?N` parameter-token mismatch (candidate / needs investigation).**
  `SQLiteConnection::Execute` substitutes **`?N` (0-indexed)** — that is the only
  parameter syntax it understands. But `MMOPersistenceSystem.cpp` prepares its
  statements with **`%1`/`%2`… (`%N`, 1-indexed)** tokens (e.g.
  `"SET character_%1 %2|1|0|…"`). Those `%N` tokens are **not** what `Execute`
  substitutes, so on the engine code path they would pass through verbatim. This
  looks like a real latent bug (all characters could collide on a literal
  `character_%1` key), **but I did not fully trace the MMO runtime path** to
  confirm impact or find a manual substitution step. Verify before relying on MMO
  persistence, and before "fixing" either side. (`candidate`)
- **Test coverage — mock vs real (as of 2026-07-07).** The `HARDEN_FLEET_HANDOFF`
  item asking for "real (non-mock) AsyncDatabase … tests" is **partly
  addressed**:

  | Test file | Exercises real code? |
  | --- | --- |
  | `Tests/TestAsyncDatabase.cpp` | **No — mock.** Standalone reimplementation of `QueryRow/QueryResult/PreparedStatementData/Transaction` in an anon namespace; never links the real types. |
  | `Tests/TestSaveSystem.cpp` | **No — mock.** Uses a local `TestBuffer`; does not touch `SaveSystem`. |
  | `Tests/TestAsyncDatabaseRegressions.cpp` | **Yes** — real `SQLiteConnection` (synchronous). |
  | `Tests/harden/Test_persistence_AsyncDatabasePool.cpp` | **Yes** — real `AsyncDatabasePool` incl. worker threads, async↔sync visibility, close/reopen. |
  | `Tests/harden/Test_persistence_AsyncDatabaseParams.cpp` | Registered in `Tests/CMakeLists.txt` as a real-path test (body not inspected here). |
  | `Tests/harden/Test_persistence_SaveSystem.cpp` | **Yes but narrow** — only `GetSaveMetadata` via hand-crafted binary files. |

- **No full Save/Load round-trip test (gap).** Nothing exercises
  `SerializeWorld`→`WriteToFile`→`ReadFromFile`→`DeserializeWorld` against a real
  ECS `World`. The `SerializeWorld` fixed-`TrySerialize`-list gotcha above is
  therefore not guarded by a test. (`open`)

---

## Provenance and maintenance

Verified by reading the following on **2026-07-07** (branch `Working`, tip
`710f797e`):
`SparkEngine/Source/Engine/SaveSystem/SaveSystem.{h,cpp}`,
`SaveSystemTypes.h`,
`SparkEngine/Source/Engine/Persistence/AsyncDatabase.{h,cpp}`,
`GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp`,
`SparkEngine/Source/Core/EngineConsoleCommands.cpp`,
`SparkEngine/Source/Core/SparkEngineWindows.cpp`, and the test files listed above.

Re-verification one-liners (run from repo root):

- Current save format version:
  `grep -n "kCurrentSaveVersion" SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp`
- The hard-coded save component walk (the two-step-rule list):
  `grep -n "TrySerialize<" SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp`
- Confirm the shared-connection threading model persists:
  `grep -n "m_syncConnection\|m_syncMutex" SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp`
- Parameter-token syntax (`?N`) in the DB layer vs `%N` in MMO:
  `grep -n "'?'" SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp` and
  `grep -n "%1\|%2" GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp`
- Which persistence tests are mocks vs real:
  `grep -rn "Standalone reimplementation\|using namespace Spark::Persistence" Tests/`
- Lint this skill with the repo/global skill-linter (see the global CLAUDE.md
  "Skill validation" note for the exact invocation).
