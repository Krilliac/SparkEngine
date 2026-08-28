# Save System

SparkEngine's save system persists ECS state and module-owned key/value state in a
versioned `.spark_save` binary file. The supported reader window is explicit:

| Contract | Version |
|---|---:|
| Oldest readable format | `kOldestSupportedSaveVersion` = 1 |
| Format written by this build | `kCurrentSaveVersion` = 2 |

Writers emit v2 only. Readers accept v1 and v2 only. A v1 file is migrated in
memory and is never rewritten merely because it was loaded.

**Primary sources:**

- `SparkEngine/Source/Engine/SaveSystem/SaveSystemTypes.h`
- `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`
- `SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp`

`ENABLE_SAVE_SYSTEM=ON`

## Runtime contract

`SaveSystem` is the singleton facade for slots, quicksave, rotating autosaves,
metadata queries, and in-memory snapshots. `ComponentSerializerRegistry` maps a
component type name to its serializer and deserializer. `SaveData` contains:

- `SaveMetadata metadata`
- `std::vector<SerializedEntity> entities`
- `std::unordered_map<std::string, std::string> customState`

The built-in component registrations cover the component types enumerated by
`SaveSystem::SerializeWorld()`. A game module may register additional callbacks,
but it must unregister module-owned callbacks before unloading its DLL.

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
They do not parse the entity payload. Metadata returned from a supported v1 file
has already passed the v1-to-v2 in-memory migration, so its version is current and
its screenshot path is empty.

## Binary format

Save files are uncompressed binary data. Multi-byte integers use the platform's
current native representation, so the certified v1/v2 compatibility fixture is
for the supported Windows x64 release profile.

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
```

### Metadata layouts

v1 and v2 share the same outer binary layout. Their metadata blocks differ:

```text
v1: saveName, sceneName, playerClass, timestamp, playTime, health,
    armor, position, kills, deaths

v2: saveName, sceneName, playerClass, screenshotPath, timestamp, playTime,
    health, armor, position, kills, deaths
```

Each listed field is newline-delimited except the three position coordinates,
which share one line. Embedded carriage returns or newlines in string metadata
are rejected by the writer.

Every entity/component/property string has a `uint16_t` length prefix. The
writer rejects oversize values instead of truncating them. The reader also caps
the save at 512 MiB, the entity count at 1,000,000, and custom-state entries at
100,000.

## Compatibility and migration

`SaveSystem::MigrateToCurrentVersion(SaveData&)` is the authoritative in-memory
migration entry point. It is transactional and idempotent:

- v1 -> v2 sets `screenshotPath` to the defined empty value and updates the
  format version;
- v2 -> v2 is a no-op;
- versions below 1 or above 2 are rejected without changing the input.

The v1 reader uses the v1 metadata layout before applying the migration. This
ordering matters: treating a v1 timestamp line as a v2 screenshot line would
shift every remaining field.

Unsupported files log the source version, the supported inclusive range, and
whether a newer or compatible older build is required. There is no unlimited
backward-compatibility promise; widening or retiring the reader window requires a
separate migration change and fixture.

## Transaction and rollback behavior

### Saving

`Save()` writes `<slot>.spark_save.tmp`, closes and durably flushes it, then
atomically replaces the destination. A failed write removes the temporary file
and leaves the previous slot in place. The local file cache is invalidated only
after the replacement succeeds.

### Loading

`Load()` performs these stages before changing caller-owned state:

1. read and bound the entire file;
2. validate magic, version, metadata, entities, components, and custom state;
3. migrate the parsed `SaveData` in memory;
4. run the optional custom-state validator;
5. restore every entity into a fresh candidate `World`;
6. replace the live `World` and `outCustomState` only after complete success.

An unknown component type or any standard/unknown exception from a registered
deserializer fails the candidate restore. The live world and custom-state output
remain unchanged.

A successful load replaces the World's registry. Callers must not retain raw
entity/component pointers across a successful load.

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

Deserializers should validate all required properties and throw on malformed
input. The SaveSystem catches the exception while the component is being restored
into the candidate world, preventing a partial live-world update.

## Compatibility evidence

The immutable v1 source fixture is:

`Tests/Fixtures/Compatibility/SaveSystem/v1-screenshotless.spark_save.hex`

The test decodes the fixture into a temporary slot; it never rewrites the source
fixture. Focused compatibility tests use the `SaveMigration_` selector and are
registered with CTest labels `compatibility;save;unit`:

```bash
ctest --test-dir build -C Release -L compatibility --output-on-failure
```

The compatibility-labeled coverage proves:

- v2 writer/header and screenshot-path round trip;
- exact, idempotent v1-to-v2 in-memory migration;
- immutable v1 read compatibility without source or slot rewrite;
- future/retired version rejection;
- unknown-component rollback;
- throwing-deserializer rollback for both World and custom state.

The same production-linked SaveSystem test file also retains the malformed-tail,
oversize-file, custom-state, and atomic slot-replacement regressions.

SAVE-230 remains broader than this save-format slice. Scene, prefab, asset,
editor-state, per-module schema, installed-build, and exact-SHA CI evidence remain
separate release gates.

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

Verified against the SAVE-230 save-format slice on 2026-08-27. The constants and
implementation named above are authoritative; this page must change in the same
commit as any save-format or compatibility-window change.
