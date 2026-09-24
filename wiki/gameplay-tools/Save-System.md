# Save System

SparkEngine's save system persists ECS state and module-owned key/value state in a
versioned `.spark_save` binary file. The supported reader window is explicit:

| Contract | Version |
|---|---:|
| Oldest readable format (N-1) | `kOldestSupportedSaveVersion` = `kCurrentSaveVersion - 1` = 3 |
| Format written by this build (N) | `kCurrentSaveVersion` = 4 |

Owner decision OD-03 fixes the window at exactly N and N-1. Writers emit v4 only.
Readers accept v3 and v4; a v3 file is migrated in memory (`MigrateToCurrentVersion`
runs the v3->v4 step) and is never rewritten merely because it was loaded. v1, v2,
and any version newer than v4 fail closed with a log line naming the file's version
and the supported window. Game modules version their own custom-state blocks the
same way through `Spark::ModulePersistedSchema` (see
[Module persisted schemas](#module-persisted-schemas)).

**Primary sources:**

- `SparkEngine/Source/Engine/SaveSystem/SaveSystemTypes.h`
- `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`
- `SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp`

The save system is compiled as part of the engine; there is no separate CMake toggle.

## Runtime contract

`SaveSystem` is the singleton facade for slots, quicksave, rotating autosaves,
metadata queries, and in-memory snapshots. `ComponentSerializerRegistry` maps a
component type name to its serializer and deserializer. `SaveData` contains:

- `SaveMetadata metadata`
- `std::vector<SerializedEntity> entities`
- `std::unordered_map<std::string, std::string> customState`

`SaveSystem::SerializeWorld()` saves every `ComponentFactory`-registered type that
owns a serializer, so module-registered serializers (see below) are reachable
without engine changes. `NameComponent` is the sole exception: it travels as
`SerializedEntity::name`, and an entity with a name but no other serializable
component is retained. A game module may register additional callbacks, but it
must unregister module-owned callbacks before unloading its DLL.

The opt-out is `SaveSystem::MarkComponentTransient(typeName)` /
`IsComponentTransient(typeName)`. The default transient set is `ProjectileComponent` and
`DecalComponent` — in-flight, regenerable state that should not be restored. Because coverage is
now registry-driven rather than a fixed 14-type allowlist, a save written by this build carries
more component types than one written before the change.

### Hierarchy

A serialized `Transform` carries a `parent` property holding the parent's index in
the saved entity list, or `"-1"` for a root. Loading rebuilds the edges through
`World::SetParent`.

### Core API

```cpp
SaveSystem& saves = SaveSystem::GetInstance();
saves.Initialize("Saves");

SaveMetadata metadata;
metadata.saveName = "Before Boss";
metadata.sceneName = "Level03";
metadata.screenshotPath = "Screenshots/slot-1.png";

std::unordered_map<std::string, std::string> gameState = {
    {"quest.main.stage", "3"},
};

saves.Save("slot-1", world, metadata, gameState);

std::unordered_map<std::string, std::string> restoredGameState;
if (!saves.Load("slot-1", world, restoredGameState))
{
    // world and restoredGameState are unchanged
}
```

Slot names are 1 to 64 characters and may contain ASCII letters, digits,
underscore, and hyphen. Files are stored as `<save-directory>/<slot>.spark_save`.
`__quicksave` and `__autosave_N` are the engine's reserved slot patterns.

## Save metadata

`SaveMetadata` contains the slot display name, scene, player class, format
version, timestamp, play time, screenshot path, and player-summary fields.
`Save()` sets the timestamp and forces the version to
`kCurrentSaveVersion`; callers cannot use it to emit an older format.

The save system stores `screenshotPath` but does not capture the image. The game
or editor must capture the thumbnail before saving and provide the path.

`GetSaveMetadata()` and `GetSaveSlots()` read only the versioned metadata block.
They do not parse the entity payload. Metadata returned from a supported v3 file
has already passed the v3-to-v4 in-memory migration, so its version is current.

## Binary format

Save files are uncompressed binary data. Legacy v1-v3 multi-byte integers use
the emitting platform's native representation; the immutable fixtures are from
the supported Windows x64 profile. The v4 writer emits fixed-width integers in
little-endian order.

```text
4 bytes   magic: "SPRK"
u32       format version
u32       metadata byte count
bytes     newline-delimited metadata block
u32       entity count
repeat entities:
  u16 + bytes  entity name
  u16          component count
  repeat components:
    u16 + bytes  component type name
    u16          property count
    repeat properties:
      u16 + bytes  key
      u16 + bytes  value
u32       custom-state count
repeat custom state:
  u16 + bytes  key
  u16 + bytes  value
v4 only:
  u32       standard CRC-32 over every preceding byte
```

The v4 CRC uses polynomial `0xEDB88320` with the standard initial/final XOR.
Readers bound the payload at the trailer, verify the complete immutable byte
snapshot before parsing or returning metadata, and reject missing, extended, or
mismatched trailers. CRC-32 detects accidental corruption; it is not keyed and
does not authenticate a save against a malicious editor.

### Metadata layout

Both readable versions (v3 and v4) use the same metadata block:

```text
saveName, sceneName, playerClass, screenshotPath, timestamp, playTime,
health, armor, position, kills, deaths
```

Each listed field is newline-delimited except the three position coordinates,
which share one line. Embedded carriage returns or newlines in string metadata
are rejected by the writer.

Every entity/component/property/custom-state key and value has a `uint16_t`
length prefix. One shared `SaveRepresentationLimits` contract applies to disk
reads/writes and public in-memory restore: 512 MiB total wire size, 64 KiB
metadata, 1,000,000 entities, `uint16_t` components per entity, `uint16_t`
properties per component, wire-derived aggregate component/property limits, and
100,000 custom-state records. `SaveRepresentationBudget` performs overflow-safe
aggregate accounting. Oversize values are rejected instead of truncated.

## Compatibility and migration

`SaveSystem::MigrateToCurrentVersion(SaveData&)` is the authoritative in-memory
migration entry point. It is transactional and idempotent:

- v3 -> v4 retains the same semantic payload and moves it into the checksummed
  v4 disk envelope;
- v4 -> v4 is a no-op;
- versions below 3 (N-1) or above 4 (N) are rejected without changing the input.

Unsupported files log the source version, the supported inclusive range, and
whether a newer or compatible older build is required. OD-03 forbids unlimited
backward compatibility: the next format bump (v5) moves the window to v4-v5, drops
the v3->v4 step, and must ship a real v4 fixture that migrates.

### Module persisted schemas

SaveSystem versions the envelope; each game module owns the meaning of the
custom-state entries it writes. A module declares one
`Spark::ModulePersistedSchema` from the public SDK header
`SparkSDK/Include/Spark/PersistedSchema.h`: its name, the custom-state key that
holds its version, and the version it writes (N). `WriteModuleSchemaVersion`
stamps N; `CheckModuleSchemaVersion` accepts exactly N and N-1 and otherwise
returns a versioned, actionable error (missing key, malformed number, older than
N-1, or newer than N). The module migrates N-1 data to N itself.

SparkGameFPS declares `FPSLocalProfile::kSchema{"SparkGameFPS",
"fps.profile.version", 1}` and uses it in the quicksave/quickload path
(`FPSLocalProfile::WriteTo` / `ReadFrom`). Schema 1 is its first schema, so its
window is 1-1 until a schema 2 adds a migration.

## Transaction and rollback behavior

### Saving

`Save()` writes `<slot>.spark_save.tmp`, closes and durably flushes it, validates
the previous revision before retaining it as `<slot>.spark_save.bak`, then
atomically replaces the destination. An unreadable primary never overwrites an
existing last-good copy. A failed write removes the temporary file and leaves
the previous slot in place. The local file cache is invalidated only after the
replacement succeeds. `SetSaveDirectory()` records the directory and the next
`Save()` creates it on demand. `DeleteSave()` removes both the slot file and its
`.bak`.

`Load()` verifies v4 CRC-32 before parsing and falls back to
`<slot>.spark_save.bak` with a logged warning when the primary is unreadable or
checksum-invalid. Save reads invalidate any prior `LocalFileCache` entry before
capturing bytes, so an externally replaced file cannot be hidden by a stale,
previously valid snapshot.

The file-based `AsyncDatabase` fallback uses the same safe publication shape for
its KV revision: it writes a sibling `.tmp`, explicitly flushes that complete
revision (`FlushFileBuffers` on Windows, `fsync` on POSIX), and only then swaps
the destination name. This prevents an interrupted write from truncating the
last readable store. It does not supply schema migrations, concurrent MMO
ownership, backup/restore rehearsal, or disaster recovery; those remain tracked
by `DATA-120`.

`SaveMetadata::slotName` is never written to disk; `GetSaveSlots()` and
`GetSaveMetadata()` populate it from the file name so callers can address the
slot they enumerated.

### Loading

`Load()` performs these stages before changing caller-owned state:

1. read and bound the entire file;
2. validate magic, version, metadata, entities, components, and custom state;
3. migrate the parsed `SaveData` in memory;
4. run the optional custom-state validator;
5. copy `customState`, restore every entity into a fresh candidate `World`, and
   prove its exact entity identifiers, entity-storage occupancy, component pools,
   and per-entity component presence match the serialized declaration;
6. resolve and validate every type-erased add/presence/remove/raw/storage-swap/
   rebind operation without touching the live registry;
7. allocate a complete immutable hierarchy-retirement plan for the live world;
8. cross the explicit live-storage boundary by preparing missing live pools;
9. retire live entities through the prebuilt `World::EntityRetirementPlan`, which
   removes hierarchy links without allocating, fires destroy observers, and clears
   per-entity event subscriptions;
10. exchange the validated component payloads and a separately constructed,
    pristine entity-storage payload so callback-mutated allocator state cannot
    affect the live world's future entity identifiers;
11. emit an explicit live `on_update` rebind for every incoming component (the
   same rebuild path consumed by all production `ReactiveSystem` subscribers);
12. exchange the staged custom-state map with a non-throwing swap.

An unknown or duplicate component type, an explicit `NameComponent` record,
duplicate property/custom-state keys, a missing/malformed/oversized required
built-in value, a failed reflected-field conversion, or any standard/unknown
exception from a registered deserializer, a deserializer that returns without
materializing its declared component, a deserializer-created entity or undeclared
staged component, transient create/destroy residue in candidate entity storage,
incomplete type-erased operations, hierarchy-plan allocation, or any
representation-limit violation fails the candidate restore. Candidate identifiers
are allocated before extension callbacks run, and the live world receives entity
allocator metadata from a separate pristine storage; a callback-only allocator
cursor change therefore cannot escape the candidate. These failures occur before
live storage preparation, so exact registry topology, hierarchy, live entities,
components, per-entity subscriptions, and custom-state output remain unchanged.

Live-pool preparation is intentionally outside the ordinary-failure catch. The
engine-owned hierarchy plan is complete before this point. If a pool allocation
or preparation callback throws, the exception propagates and an empty pool may
remain in the registry. Retirement has not begun, so entity and component
payloads, per-entity EventBus subscriptions, and custom-state output remain
unchanged, but exact topology/rollback is no longer promised and the failure is
never misreported as `false`.

The current `World` API exposes EnTT lifecycle sinks whose callbacks are not
required to be non-throwing. An exception from `on_destroy` after retirement
begins cannot be rolled back together with observer-owned state and EventBus
side effects. Such an exception therefore propagates instead of being reported
as an ordinary `false`; the caller must treat it as a fatal lifecycle programming
error and must not continue using the potentially partially retired `World`.
SAVE-230 remains blocked on a larger ownership/rebind design if recoverable
rollback across arbitrary lifecycle observers is required.

A successful load preserves the live EnTT registry and its registry-bound signal
observers. Incoming entity identifiers are installed only after subscriptions
for retired/colliding identifiers have been removed. Entity/component pointers
and entity-lifetime assumptions remain invalid across a successful load.

## Custom component registration

```cpp
auto& registry = ComponentSerializerRegistry::GetInstance();
registry.Register(
    "Inventory",
    [](const void* raw)
    {
        const auto& inventory = *static_cast<const Inventory*>(raw);
        return SerializedComponent{"Inventory", {{"gold", std::to_string(inventory.gold)}}};
    },
    [](World& world, EntityID entity, const SerializedComponent& encoded)
    {
        Inventory inventory;
        inventory.gold = std::stoi(encoded.properties.at("gold"));
        world.AddComponent<Inventory>(entity, inventory);
    });
```

Custom component types must also register matching `ComponentFactory` operations,
including the transactional `prepareStorage`, `swapStorageContents`, and
`notifyRebound` callbacks;
the engine's `SPARK_REGISTER_COMPONENT` registrations are the reference pattern.
Without those operations, loading the custom component fails closed before the
live world is retired. Deserializers must validate all required properties and
throw on malformed input. The SaveSystem catches the exception while the
component is being restored into the candidate world, preventing a partial
live-world update.

## Compatibility evidence

The N-1 fixture that must migrate is the production-generated v3 FPS save:

`Tests/Fixtures/Compatibility/SaveSystem/v3-fps-profile.spark_save.hex`

The v1 and v2 fixtures stay committed as real pre-window files:

`Tests/Fixtures/Compatibility/SaveSystem/v1-screenshotless.spark_save.hex`
`Tests/Fixtures/Compatibility/SaveSystem/v2-screenshot-without-hierarchy.spark_save.hex`

Under OD-03 every read path (`GetSaveMetadata`, `Load`) must refuse them without
changing the caller's metadata, world, or custom state, and without rewriting the
copied slot or the fixture. The v3 test also reads the FPS module's own
`fps.profile.*` block through the production `FPSLocalProfile::ReadFrom`.
Focused compatibility tests use the `SaveMigration_` selector (CTest
`SparkSaveCompatibilityTests`, labels `compatibility;save;unit`); editor scene
compatibility uses `SceneMigration_` (`SparkSceneCompatibilityTests`, labels
`compatibility;scene;unit`, fixtures under `Tests/Fixtures/Compatibility/SceneFile/`):

```bash
ctest --test-dir build -C Release -L compatibility --output-on-failure --no-tests=error
```

The compatibility-labeled coverage includes:

- current-writer header and screenshot-path round trip;
- exact, idempotent v3-to-v4 (N-1 to N) in-memory migration, with v2 and v5
  snapshots rejected unchanged;
- immutable v1 and v2 fixtures refused on every read path without mutation;
- immutable production-generated v3 FPS-profile compatibility without source or
  slot rewrite, including the module-owned profile schema;
- module persisted-schema declarations accepting exactly N and N-1;
- v4 writer/trailer round-trip and exact CRC verification;
- payload, trailer, and version-field corruption rejection before metadata or
  world mutation, including cache-fresh external replacement;
- checksum-invalid primary recovery through a valid retained copy;
- future/retired version rejection;
- successful lifecycle commit with registry-observer retention and stale
  entity-subscription removal, plus explicit incoming reactive rebinds;
- fail-closed missing, malformed, oversized, and reflected component values;
- fail-closed duplicate component/property/custom-state records and explicit
  `NameComponent` records, without entering entity lifecycle;
- shared disk/in-memory size and count boundaries, including exact/max+1 string
  and overflow-safe synthetic aggregate cases;
- a custom deserializer that omits its declared component, with exact live
  registry topology, entity/component, EventBus, and custom-state preservation;
- custom deserializers that create ghost entities or extra staged components,
  rejected before live storage preparation with exact live-state preservation;
- transient create/destroy candidate entities rejected even when the surviving
  active IDs and component topology match, plus allocator-cursor mutation isolated
  by the pristine committed entity storage;
- hierarchy-plan failpoint rollback and a boundary guard proving every live
  `Transform::children` copy occurs before storage preparation;
- propagation of live-storage-preparation failure after an empty pool appears,
  proving no catch reports a false unchanged-topology rollback;
- unknown-component rollback;
- throwing-deserializer rollback for both World and custom state;
- propagation of throwing lifecycle observers instead of a false unchanged-world
  result after commit has begun.

The same production-linked SaveSystem test file also retains the malformed-tail,
oversize-file, custom-state, and atomic slot-replacement regressions.

SAVE-230 remains broader than this save-format slice. Local production-linked
tests now cover the v4 CRC envelope, the OD-03 N/N-1 window (v3 migrates; v1/v2
fixtures fail closed), SceneFile v1-to-v2 migration from real v1 fixtures, the
module persisted-schema mechanism used by SparkGameFPS, transactional corruption
rejection, cache freshness, and primary-to-backup recovery. The staged MinSizeRel
FPS smoke separately demonstrates same-version progression XP persistence across
two fresh D3D11 WARP processes. Still open: the rest of `FPSLocalProfile`, forced
process-interruption rehearsal, prefab/asset/editor-state migrations, schema
declarations for the other game modules, clean-machine installation, and hosted
exact-SHA evidence. CRC-32 is not an authenticity control. The ordinary
build workflows run compatibility tests serially with the rest of the suite; no
dedicated compatibility CI job is claimed.

## Threading

`SaveSystem` and ECS world access are main-thread operations. The registry must
not be mutated concurrently with save or load. `WriteToFile()` and
`ReadFromFile()` are internal implementation details; callers should use the
public API rather than invoking background I/O against singleton state.

## See also

- [Entity Component System](../subsystems/Entity-Component-System.md)
- [Asset Migration](Asset-Migration.md)
- [Scene Management](../subsystems/Scene-Management.md)
- [Persistence System](Persistence-System.md)

## Source & Freshness

Updated against the SAVE-230 v4 integrity/migration and installed FPS persistence
slices on 2026-09-21. The constants and implementation named above are
authoritative; this page must change in the same commit as any save-format or
compatibility-window change.
