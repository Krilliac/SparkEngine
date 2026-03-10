# SparkEngine Save System — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/SaveSystem/` (SaveSystem, ComponentSerializerRegistry)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `SaveSystem/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Save subsystem provides ECS-aware game state persistence. It serializes the entire World (entities + components) to compressed JSON via a `ComponentSerializerRegistry` that maps type names to serialize/deserialize function pairs. The system supports named save slots, quicksave/quickload, rotating autosave, and console integration. `SaveSystem` is a singleton with a well-documented API including in-memory snapshots via `SerializeWorld()`/`DeserializeWorld()`. However, there are concerns about missing dependencies, platform-specific types in portable data, and no corruption protection.

---

## Critical Gaps

### GAP-SS01 — RapidJSON and Miniz Dependencies Not Visible

**Files**:
- `SaveSystem/SaveSystem.h` (lines 520–521, doc comment references "JSON compressed with miniz deflate")
- Project `CMakeLists.txt` files
- `ThirdParty/` or dependency directories

**Impact**: The `SaveSystem` documentation states it uses "RapidJSON" for JSON serialization and "miniz" for deflate compression. However, neither library appears as a dependency in the CMake build system or third-party directory. If these libraries are not actually linked, `WriteToFile()` and `ReadFromFile()` — the core I/O methods — may be stubbed or non-functional.

**Evidence**: No `find_package(RapidJSON)`, `add_subdirectory(miniz)`, or similar CMake calls found. No `rapidjson/` or `miniz/` directory in the source tree. The `.h` file does not `#include` any RapidJSON or miniz headers.

**What is needed**: Either integrate RapidJSON and miniz as actual dependencies (add to CMake, include headers in `.cpp` file), or switch to a header-only JSON library (e.g., nlohmann/json) and zlib/miniz. Verify that `WriteToFile()`/`ReadFromFile()` actually compile and produce valid save files.

---

## Major Gaps

### GAP-SS02 — Platform-Dependent Types in Save Metadata

**Files**:
- `SaveSystem/SaveSystem.h` (line 198, `DirectX::XMFLOAT3 playerPosition`)

**Impact**: `SaveMetadata` contains `DirectX::XMFLOAT3 playerPosition`, a Windows-only type. Save files written on Windows cannot be read on Linux/macOS (the type doesn't exist), and the `SaveSystem.h` header fails to compile on non-Windows platforms. This makes save files non-portable across platforms.

**Evidence**: `#include <DirectXMath.h>` at line 79, guarded by `SPARK_PLATFORM_WINDOWS`. `XMFLOAT3` usage at line 198 is outside the guard, causing compile errors on non-Windows.

**What is needed**: Replace `DirectX::XMFLOAT3 playerPosition` with a platform-agnostic type (e.g., three `float` members, or the engine's cross-platform math stub from `Platform.h`).

---

### GAP-SS03 — No Save Corruption Detection

**Files**:
- `SaveSystem/SaveSystem.h` (lines 807–820, `WriteToFile`/`ReadFromFile`)

**Impact**: Save files have no integrity verification. If a save file is partially written (power failure, crash during save), truncated, or corrupted on disk, `ReadFromFile()` will attempt to decompress and parse invalid data. The result is undefined — likely a crash, silent data loss, or a partially loaded world. There is no checksum, magic number, or file header to detect corruption before parsing.

**What is needed**: Add a file header with a magic number (e.g., `"SPRK"`) and a CRC32/SHA256 checksum of the compressed data. Verify the checksum before decompression. Use atomic writes (write to temp file, then rename) to prevent partial saves.

---

### GAP-SS04 — Singleton Pattern Outside EngineContext

**Files**:
- `SaveSystem/SaveSystem.h` (lines 577, `SaveSystem::GetInstance()`)

**Impact**: `SaveSystem` uses the singleton pattern but is not registered in `EngineContext`. Game modules access it via `SaveSystem::GetInstance()` directly, bypassing the service locator. This creates a hidden global dependency and makes testing difficult.

**Evidence**: `EngineContext.h` has no `GetSaveSystem()` method. `IEngineContext` does not expose the save system.

**What is needed**: Register `SaveSystem` in `EngineContext` and expose through `IEngineContext`.

---

## Moderate Gaps

### GAP-SS05 — Version Migration System Documented But Not Implemented

**Files**:
- `SaveSystem/SaveSystem.h` (lines 149–155, `version` field; lines 615–617, Load doc comment)

**Impact**: `SaveMetadata::version` defaults to 1, and the `Load()` documentation mentions "applies version migrations if the save format version differs from the current engine version." However, there is no migration code, no migration registry, and no mechanism to upgrade old save files. If the save format changes, old saves will fail to load with no recovery path.

**What is needed**: Implement a migration registry: `RegisterMigration(fromVersion, toVersion, migrationFunc)`. In `Load()`, check `metadata.version` against the current version and apply migrations in sequence. At minimum, log a clear error when version mismatch is detected.

---

### GAP-SS06 — Not Thread-Safe, No Async Save Option

**Files**:
- `SaveSystem/SaveSystem.h` (lines 537–541, thread safety documentation)

**Impact**: The documentation explicitly states "SaveSystem is not thread-safe" and suggests serializing on the main thread then writing from a background thread. However, no helper method or utility is provided for this pattern. For large game worlds, `Save()` on the main thread causes a frame stutter. The suggested workaround requires game code to manually call `SerializeWorld()` and then manage a background thread for `WriteToFile()`.

**What is needed**: Add an `AsyncSave()` method that serializes on the main thread and queues the file write to a worker thread. Return a future or callback for completion notification.

---

### GAP-SS07 — No Cloud Save Support

**Files**: All save system files

**Impact**: Save files are written to the local filesystem only. No abstraction exists for cloud save backends (Steam Cloud, Epic Online Services, Xbox Live, etc.). Games shipping on multiple storefronts must implement cloud save separately.

**What is needed**: Abstract the storage backend behind an interface (`ISaveStorage`) with `Write()`, `Read()`, `Exists()`, `Delete()`, `List()` methods. Provide a `LocalFileSaveStorage` implementation (current behavior) and allow game code to plug in platform-specific cloud storage implementations.

---

### GAP-SS08 — RegisterBuiltins() Scope Unclear

**Files**:
- `SaveSystem/SaveSystem.h` (lines 479–486, `RegisterBuiltins()`)

**Impact**: `RegisterBuiltins()` is documented as registering "Transform, NameComponent, HealthComponent, RigidBodyComponent, MeshRenderer, Camera, AudioSourceComponent, LightComponent, AnimationController, AIComponent, and more." The vague "and more" makes it unclear which components are actually serializable. If a component type is not registered, it is silently skipped during save — the developer has no way to know data was lost.

**What is needed**: Enumerate all registered component types explicitly in documentation or provide a `ListRegisteredTypes()` method. Log a warning when saving an entity with components that have no registered serializer.

---

## Minor Gaps

### GAP-SS09 — No Save File Size Limits or Warnings

**Files**: All save system files

**Impact**: No check on save file size. A world with thousands of entities could produce multi-megabyte save files without warning. No limit prevents filling disk space.

**What is needed**: Log a warning when save file size exceeds a configurable threshold (e.g., 100MB). Optionally set a maximum save file size with an error if exceeded.

---

### GAP-SS10 — CustomState Is Stringly-Typed

**Files**:
- `SaveSystem/SaveSystem.h` (lines 336–349, `SaveData::customState`)

**Impact**: `customState` is `std::unordered_map<std::string, std::string>` — all values are strings. Storing numeric, boolean, or structured data requires manual string conversion. There is no validation or schema for custom state keys.

**What is needed**: Consider using a `std::variant<std::string, int, float, bool>` value type, or document the recommended encoding conventions. This is a minor ergonomic issue.

---
