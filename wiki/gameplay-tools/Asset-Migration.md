# Asset Migration

The Asset Migration system provides automatic forward-migration of serialized asset files across engine versions. Each binary asset carries an `AssetFileHeader` with the magic bytes `SPRK` and a semantic version; the `AssetMigrationRegistry` chains registered `IMigrationStep` implementations to transform data from any older version to the current one.

**Source:** `SparkEngine/Source/Core/AssetMigration.h`

## Overview

| Class / Struct | Responsibility |
|---|---|
| `AssetMigrationRegistry` | Singleton registry that stores migration steps, resolves migration paths, and applies them to in-memory asset buffers |
| `IMigrationStep` | Abstract interface for a single version-to-version transformation step |
| `AssetFileHeader` | Fixed-size binary header at the start of every SparkEngine asset file (magic, version, type, checksum, sizes) |
| `AssetVersion` | Semantic version triplet (`major.minor.patch`) with spaceship comparison |
| `AssetType` | Enum discriminating the kind of asset stored in a binary file |

## Key Enums and Types

### AssetType

```cpp
enum class AssetType : uint8_t
{
    Scene,      // World / level scene
    Material,   // Material definition
    Prefab,     // Entity prefab template
    SaveGame,   // Player save data
    Archetype,  // ECS archetype descriptor
    Config,     // Engine or game configuration
    ShaderCache // Precompiled shader cache
};
```

### AssetVersion

```cpp
struct AssetVersion
{
    uint16_t major = 1;
    uint16_t minor = 0;
    uint16_t patch = 0;

    constexpr auto operator<=>(const AssetVersion&) const = default;
    constexpr bool operator==(const AssetVersion&) const = default;

    std::string ToString() const; // Returns "major.minor.patch"
};
```

Supports full three-way comparison via `<=>`, so versions can be sorted and compared naturally:

```cpp
Spark::AssetVersion v1{1, 0, 0};
Spark::AssetVersion v2{1, 1, 0};
assert(v1 < v2);
assert(v2.ToString() == "1.1.0");
```

### AssetFileHeader

Every SparkEngine binary asset begins with this fixed-size header:

```cpp
struct AssetFileHeader
{
    uint32_t magic = 0x5350524B;   // "SPRK" in little-endian
    AssetVersion version{1, 0, 0}; // Format version of the payload
    AssetType assetType = AssetType::Scene;
    uint32_t checksum = 0;         // CRC32 of the payload (after header)
    uint32_t headerSize = 0;       // Total header size in bytes (for forward compat)
    uint64_t dataSize = 0;         // Payload size in bytes (after header)
};
```

### Binary File Layout

```
+------------------+-----------------------------------+
| AssetFileHeader  | Payload (opaque bytes)            |
| magic: "SPRK"   | Interpreted by owning subsystem   |
| version: 1.2.0  |                                   |
| type: Scene      |                                   |
| checksum: CRC32  |                                   |
| headerSize: N    |                                   |
| dataSize: M      |                                   |
+------------------+-----------------------------------+
  ^--- N bytes ---^  ^---------- M bytes ------------>
```

## Quick Start

### Registering a migration and migrating an asset

```cpp
#include "Core/AssetMigration.h"

auto& registry = Spark::AssetMigrationRegistry::GetInstance();
registry.Initialize();

// Register a custom migration step (see "Writing Custom Migrations" below)
registry.RegisterMigration(std::make_unique<SceneV1ToV2>());
registry.SetCurrentVersion(Spark::AssetType::Scene, {2, 0, 0});

// Load a file into memory
std::vector<uint8_t> data = LoadFile("level.scene");

// Migrate in place
if (registry.MigrateAsset(data, Spark::AssetType::Scene))
{
    SaveFile("level.scene", data);
    Log::Info("Migration", "level.scene migrated successfully");
}
else
{
    Log::Error("Migration", "Migration failed for level.scene");
}
```

### Checking if migration is needed

```cpp
Spark::AssetFileHeader header;
std::memcpy(&header, data.data(), sizeof(header));

if (registry.NeedsMigration(header))
{
    Log::Info("Migration", "Asset at v{} needs migration to v{}",
              header.version.ToString(),
              registry.GetCurrentVersion(header.assetType).ToString());
}
```

### Inspecting the migration path

```cpp
auto path = registry.GetMigrationPath(
    Spark::AssetVersion{1, 0, 0},  // from
    Spark::AssetVersion{3, 0, 0},  // to
    Spark::AssetType::Scene
);

for (auto* step : path)
{
    Log::Info("Migration", "  {} -> {}: {}",
              step->GetSourceVersion().ToString(),
              step->GetTargetVersion().ToString(),
              step->GetDescription());
}
```

### Validating a header

```cpp
Spark::AssetFileHeader header;
std::memcpy(&header, data.data(), sizeof(header));

if (!Spark::ValidateHeader(header))
{
    Log::Error("Asset", "Invalid header: bad magic or corrupted data");
}
```

## Writing Custom Migrations

Implement `IMigrationStep` for each version-to-version transformation. Each step reads from a `BinaryReader` (source version format) and writes to a `BinaryWriter` (target version format).

```cpp
#include "Core/AssetMigration.h"
#include "Utils/Serializer.h"

class SceneV1ToV2 final : public Spark::IMigrationStep
{
public:
    Spark::AssetVersion GetSourceVersion() const override { return {1, 0, 0}; }
    Spark::AssetVersion GetTargetVersion() const override { return {2, 0, 0}; }

    bool Migrate(Spark::BinaryReader& reader, Spark::BinaryWriter& writer) override
    {
        // Read v1 format: entity count + flat entity list
        uint32_t entityCount = reader.Read<uint32_t>();

        // Write v2 format: entity count + per-entity size prefix
        writer.Write(entityCount);
        for (uint32_t i = 0; i < entityCount; ++i)
        {
            auto name = reader.ReadString();
            float x = reader.Read<float>();
            float y = reader.Read<float>();
            float z = reader.Read<float>();

            // v2 adds a size prefix and a rotation quaternion (default identity)
            uint32_t entitySize = /* calculate */ 0;
            writer.Write(entitySize);
            writer.WriteString(name);
            writer.Write(x);
            writer.Write(y);
            writer.Write(z);
            // New in v2: rotation quaternion
            writer.Write(0.0f); // qx
            writer.Write(0.0f); // qy
            writer.Write(0.0f); // qz
            writer.Write(1.0f); // qw
        }
        return true;
    }

    std::string_view GetDescription() const override
    {
        return "Add rotation quaternion to scene entities";
    }
};
```

### Chaining multiple steps

Register steps for each version hop. The registry automatically finds the shortest path:

```cpp
registry.RegisterMigration(std::make_unique<SceneV1ToV2>()); // 1.0.0 -> 2.0.0
registry.RegisterMigration(std::make_unique<SceneV2ToV3>()); // 2.0.0 -> 3.0.0
registry.SetCurrentVersion(Spark::AssetType::Scene, {3, 0, 0});

// An asset at v1.0.0 will be migrated through v1 -> v2 -> v3 automatically
registry.MigrateAsset(data, Spark::AssetType::Scene);
```

### Using minor/patch versions

The version system supports granular versioning:

```cpp
registry.RegisterMigration(std::make_unique<MaterialV1_0ToV1_1>()); // 1.0.0 -> 1.1.0
registry.RegisterMigration(std::make_unique<MaterialV1_1ToV1_2>()); // 1.1.0 -> 1.2.0
registry.RegisterMigration(std::make_unique<MaterialV1_2ToV2_0>()); // 1.2.0 -> 2.0.0
registry.SetCurrentVersion(Spark::AssetType::Material, {2, 0, 0});
```

## Configuration

### Current Version Defaults

After `Initialize()`, all asset types default to version `1.0.0`. Call `SetCurrentVersion()` to set the actual current version for each type your project uses:

```cpp
registry.SetCurrentVersion(Spark::AssetType::Scene,      {3, 0, 0});
registry.SetCurrentVersion(Spark::AssetType::Material,   {2, 1, 0});
registry.SetCurrentVersion(Spark::AssetType::SaveGame,   {1, 5, 0});
registry.SetCurrentVersion(Spark::AssetType::ShaderCache,{1, 0, 0});
```

When `RegisterMigration()` is called, the target version of the step auto-bumps the current version for all types if it is higher than the existing current version. Prefer explicit `SetCurrentVersion()` calls over relying on auto-bump.

### CRC32 Checksums

The `ComputeCRC32()` function uses the standard 0xEDB88320 polynomial (same as zlib). After migration, `MigrateAsset()` automatically recalculates the checksum on the new payload:

```cpp
uint32_t crc = Spark::ComputeCRC32(payload.data(), payload.size());
```

## Console Commands

| Method | Description |
|---|---|
| `Console_GetStatus()` | Returns initialization state, step count, and per-type current versions |

Example output:

```
AssetMigrationRegistry: initialized, 5 step(s) registered
  Scene: v3.0.0
  Material: v2.1.0
  Prefab: v1.0.0
  SaveGame: v1.5.0
  Archetype: v1.0.0
  Config: v1.0.0
  ShaderCache: v1.0.0
```

## Integration

### With AssetValidator

Run migration before validation to ensure assets are in the expected format:

```cpp
auto& migration = Spark::AssetMigrationRegistry::GetInstance();
migration.Initialize();
// Register steps...

// Migrate, then validate
std::vector<uint8_t> data = LoadFile("level.scene");
migration.MigrateAsset(data, Spark::AssetType::Scene);
SaveFile("level.scene", data);

auto& validator = Spark::AssetValidator::GetInstance();
auto report = validator.ValidateFile("level.scene");
```

### With GamePackager

Migrate all assets before packaging to ensure the distributable build contains only current-version assets. See [Game-Packaging](Game-Packaging.md).

### With BinaryReader / BinaryWriter

Migration steps use `BinaryReader` and `BinaryWriter` from `Utils/Serializer.h`. Callers that invoke `ReadHeader()`, `WriteHeader()`, or `MigrateAsset()` must include `Serializer.h`.

```cpp
#include "Core/AssetMigration.h"
#include "Utils/Serializer.h"

// ReadHeader and WriteHeader are available after both includes
Spark::BinaryReader reader(data);
auto headerResult = Spark::ReadHeader(reader);
if (headerResult.has_value())
{
    auto header = headerResult.value();
    // Process header...
}
```

### With SaveSystem

Player save files use `AssetType::SaveGame`. Register save format migrations so older saves load correctly after engine updates:

```cpp
registry.RegisterMigration(std::make_unique<SaveV1ToV2>());
registry.SetCurrentVersion(Spark::AssetType::SaveGame, {2, 0, 0});
```

## API Reference

### AssetMigrationRegistry

| Method | Signature | Description |
|---|---|---|
| `GetInstance` | `static AssetMigrationRegistry& GetInstance()` | Get the singleton instance |
| `Initialize` | `void Initialize()` | Initialize registry, clear previous state, set all types to v1.0.0 |
| `Shutdown` | `void Shutdown()` | Release resources |
| `RegisterMigration` | `void RegisterMigration(std::unique_ptr<IMigrationStep> step)` | Register a migration step |
| `SetCurrentVersion` | `void SetCurrentVersion(AssetType type, AssetVersion ver)` | Set the current version for an asset type |
| `GetCurrentVersion` | `AssetVersion GetCurrentVersion(AssetType type) const` | Get the current version (defaults to 1.0.0) |
| `NeedsMigration` | `bool NeedsMigration(const AssetFileHeader& header) const` | Check if a file header's version is behind current |
| `GetMigrationPath` | `std::vector<IMigrationStep*> GetMigrationPath(AssetVersion from, AssetVersion to, AssetType type) const` | Compute ordered chain of steps between versions |
| `MigrateAsset` | `bool MigrateAsset(std::vector<uint8_t>& data, AssetType type) const` | Migrate an in-memory buffer to current version |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Human-readable status string |

### IMigrationStep

| Method | Signature | Description |
|---|---|---|
| `GetSourceVersion` | `virtual AssetVersion GetSourceVersion() const = 0` | Version this step reads from |
| `GetTargetVersion` | `virtual AssetVersion GetTargetVersion() const = 0` | Version this step produces |
| `Migrate` | `virtual bool Migrate(BinaryReader& reader, BinaryWriter& writer) = 0` | Transform payload data between versions |
| `GetDescription` | `virtual std::string_view GetDescription() const = 0` | Human-readable description of what changes |

### Free Functions

| Function | Signature | Description |
|---|---|---|
| `ComputeCRC32` | `uint32_t ComputeCRC32(const uint8_t* data, size_t size)` | Standard CRC32 (zlib polynomial) |
| `WriteHeader` | `void WriteHeader(BinaryWriter& writer, AssetType type)` | Serialize an AssetFileHeader |
| `ReadHeader` | `std::expected<AssetFileHeader, std::string> ReadHeader(BinaryReader& reader)` | Deserialize and validate a header |
| `ValidateHeader` | `bool ValidateHeader(const AssetFileHeader& header)` | Quick sanity check on magic and invariants |

## Thread Safety

`AssetMigrationRegistry` is **not thread-safe**. `RegisterMigration()` and `MigrateAsset()` must not be called concurrently. The singleton access via `GetInstance()` is safe (function-local static), but all mutating operations must be externally synchronized.

`MigrateAsset()` modifies the passed `std::vector<uint8_t>` in place. Do not share the same buffer across threads during migration.

Individual `IMigrationStep` implementations should be stateless or internally synchronized if the same step instance could be used concurrently (unlikely in practice, since the registry serializes step execution).

## See Also

- [Asset-Validation](Asset-Validation.md) -- Validate assets after migration
- [Game-Packaging](Game-Packaging.md) -- Package migrated assets for distribution
- [Asset-Format-Specifications](../specifications/Asset-Format-Specifications.md) -- Detailed binary format documentation
- [Asset-Pipeline](Asset-Pipeline.md) -- Overall asset import/export workflow
