# Runtime Prefabs

The Runtime Prefab system provides reusable, data-driven entity templates that can be registered, cloned, serialized, and instantiated at runtime. Each prefab holds a list of component descriptors with string key-value properties, and a global `PrefabRegistry` manages lookup by name or numeric ID and handles spawning via the ECS.

**Source:** `SparkEngine/Source/Engine/ECS/RuntimePrefab.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `RuntimePrefab` | A reusable entity template holding ordered component descriptors with property overrides |
| `PrefabComponentData` | Serialized descriptor for a single component (type name + key-value properties) |
| `PrefabRegistry` | Singleton registry that stores, retrieves, and spawns entities from registered prefabs |
| `PrefabFileHeader` | Binary file format constants (magic number and version) for prefab serialization |

## Key Types

### PrefabComponentData

Describes a single component within a prefab. Properties are stored as string key-value pairs that the spawning code interprets when creating real ECS components:

```cpp
struct PrefabComponentData
{
    std::string typeName;  // Component type (e.g. "Transform", "MeshRenderer")
    std::unordered_map<std::string, std::string> properties;  // Key-value pairs
};
```

### PrefabFileHeader

Constants for the binary serialization format:

```cpp
struct PrefabFileHeader
{
    static constexpr uint32_t kMagic = 0x50524642;  // "PRFB"
    static constexpr uint32_t kVersion = 1;
};
```

## Quick Start

### Creating a Prefab

```cpp
#include "Engine/ECS/RuntimePrefab.h"

// Create a prefab for an enemy guard entity
auto prefab = std::make_unique<Spark::ECS::RuntimePrefab>("EnemyGuard");

// Add component descriptors with properties
prefab->AddComponent("Transform", {
    {"posX", "0"}, {"posY", "0"}, {"posZ", "0"},
    {"rotY", "180"}
});

prefab->AddComponent("MeshRenderer", {
    {"mesh", "models/guard.fbx"},
    {"material", "materials/guard_pbr.mat"}
});

prefab->AddComponent("Health", {
    {"maxHealth", "100"},
    {"currentHealth", "100"}
});

prefab->AddComponent("AIController", {
    {"behaviorTree", "ai/guard_patrol.bt"},
    {"detectionRange", "15.0"}
});
```

### Registering Prefabs

```cpp
auto& registry = Spark::ECS::PrefabRegistry::GetInstance();

// RegisterPrefab takes ownership and returns an auto-assigned ID
uint32_t guardId = registry.RegisterPrefab(std::move(prefab));

// Register more prefabs
auto crate = std::make_unique<Spark::ECS::RuntimePrefab>("WoodenCrate");
crate->AddComponent("Transform", {{"posX", "0"}, {"posY", "0"}});
crate->AddComponent("MeshRenderer", {{"mesh", "models/crate.fbx"}});
crate->AddComponent("Physics", {{"mass", "10.0"}, {"shape", "box"}});
uint32_t crateId = registry.RegisterPrefab(std::move(crate));
```

### Spawning Entities

```cpp
// Spawn by ID
uint32_t entity1 = registry.SpawnEntity(guardId);

// Spawn by name
uint32_t entity2 = registry.SpawnEntity("WoodenCrate");

// Spawn multiple instances
for (int i = 0; i < 10; ++i)
{
    uint32_t entity = registry.SpawnEntity("EnemyGuard");
    // Each call creates a new ECS entity with the prefab's components
}
```

### Querying the Registry

```cpp
// Check how many prefabs are registered
uint32_t count = registry.GetRegisteredCount();

// Get all registered prefab names
auto names = registry.GetAllPrefabNames();
for (auto name : names)
{
    std::println("Registered prefab: {}", name);
}

// Look up a specific prefab
const auto* guardPrefab = registry.GetPrefab("EnemyGuard");
if (guardPrefab)
{
    std::println("Guard has {} components", guardPrefab->GetComponents().size());
}

// Look up by ID
const auto* prefabById = registry.GetPrefab(guardId);
```

### Component Inspection

```cpp
const auto* prefab = registry.GetPrefab("EnemyGuard");

// Check if a component type exists
if (prefab->HasComponent("AIController"))
{
    std::println("This prefab has AI capabilities");
}

// Iterate over all component descriptors
for (const auto& comp : prefab->GetComponents())
{
    std::println("Component: {}", comp.typeName);
    for (const auto& [key, value] : comp.properties)
    {
        std::println("  {} = {}", key, value);
    }
}
```

### Nested Prefabs (Inheritance)

Prefabs support inheritance through a parent pointer. When spawning, the parent's components are applied first, then the child's components override or extend them:

```cpp
// Base enemy prefab
auto baseEnemy = std::make_unique<Spark::ECS::RuntimePrefab>("BaseEnemy");
baseEnemy->AddComponent("Transform", {{"posX", "0"}, {"posY", "0"}});
baseEnemy->AddComponent("Health", {{"maxHealth", "100"}});
uint32_t baseId = registry.RegisterPrefab(std::move(baseEnemy));

// Specialized prefab inheriting from base
auto boss = std::make_unique<Spark::ECS::RuntimePrefab>("BossEnemy");
boss->SetParent(registry.GetPrefab(baseId));
boss->AddComponent("Health", {{"maxHealth", "500"}});  // Override health
boss->AddComponent("BossAI", {{"phase", "1"}});         // Add new component
uint32_t bossId = registry.RegisterPrefab(std::move(boss));

// Query parent
const auto* bossPrefab = registry.GetPrefab(bossId);
if (bossPrefab->GetParent() != nullptr)
{
    std::println("Boss inherits from: {}", bossPrefab->GetParent()->GetName());
}
```

### Cloning Prefabs

Create deep copies of existing prefabs for modification:

```cpp
const auto* original = registry.GetPrefab("EnemyGuard");
auto variant = original->Clone();

// Modify the clone
variant->RemoveComponent("AIController");
variant->AddComponent("AIController", {
    {"behaviorTree", "ai/guard_aggressive.bt"},
    {"detectionRange", "25.0"}
});

uint32_t variantId = registry.RegisterPrefab(std::move(variant));
```

## Serialization

### Binary Serialization

Prefabs serialize to a binary format with a `PrefabFileHeader` (magic `0x50524642` / "PRFB", version 1), followed by the prefab name, component count, and each component's type name and properties:

```cpp
// Serialize a single prefab
Spark::BinaryWriter writer("prefabs/guard.prefab");
prefab->Serialize(writer);

// Deserialize
Spark::BinaryReader reader("prefabs/guard.prefab");
auto loaded = std::make_unique<Spark::ECS::RuntimePrefab>("");
loaded->Deserialize(reader);
```

### Registry File I/O

Save and load all registered prefabs at once:

```cpp
// Save all registered prefabs to a binary file
registry.SaveToFile("prefabs/all_prefabs.bin");

// Load prefabs from a binary file and register them
registry.LoadFromFile("prefabs/all_prefabs.bin");
```

### Property Overrides at Spawn Time

Since properties are string key-value pairs, you can clone a prefab, modify properties, and spawn without registering the variant:

```cpp
const auto* basePrefab = registry.GetPrefab("EnemyGuard");
auto instance = basePrefab->Clone();

// Override a specific property before spawning
// (Requires accessing the component data directly)
for (auto& comp : /* mutable access to components */)
{
    if (comp.typeName == "Transform")
    {
        comp.properties["posX"] = "50.0";
        comp.properties["posZ"] = "30.0";
    }
}
```

## Console Commands

The `PrefabRegistry` provides a console status method:

```cpp
std::string status = registry.Console_GetStatus();
// Output:
// [PrefabRegistry] prefabs=3
//   [1] EnemyGuard (4 components)
//   [2] WoodenCrate (3 components)
//   [3] BossEnemy (2 components)
```

## Configuration

### Prefab ID Assignment

Prefab IDs are auto-incremented starting from 1. The registry uses two indices:

- `m_prefabs`: `uint32_t` ID to `unique_ptr<RuntimePrefab>` mapping
- `m_nameIndex`: `string` name to `uint32_t` ID for fast name-based lookup

Entity IDs from `SpawnEntity()` start at 1000 (placeholder counter; production uses the EnTT registry).

### Removing Prefabs

```cpp
// Remove by ID
bool removed = registry.RemovePrefab(guardId);

// Note: RemovePrefab cleans up both the ID mapping and name index
```

## Integration

### With the ECS

The spawn path creates EnTT entities and attaches components based on the prefab's `PrefabComponentData` list. Parent prefab components are applied first when `GetParent()` returns non-null:

```cpp
// The internal SpawnFromPrefab() method:
// 1. Creates an EnTT entity
// 2. Walks prefab.GetComponents() and adds matching components
// 3. Applies parent prefab components first if GetParent() != nullptr
```

### With the Editor

The editor can display registered prefabs in an asset browser, allowing drag-and-drop instantiation:

```cpp
// Editor lists all available prefabs
auto names = registry.GetAllPrefabNames();
for (auto name : names)
{
    if (ImGui::Selectable(std::string(name).c_str()))
    {
        registry.SpawnEntity(name);
    }
}
```

### With the Serializer

`RuntimePrefab` uses `BinaryWriter` and `BinaryReader` for serialization. The format is versioned via `PrefabFileHeader::kVersion` for forward compatibility.

### With the Streaming System

Prefabs can be loaded on demand when streaming new areas:

```cpp
// When an area streams in, load its prefab definitions
registry.LoadFromFile("Areas/Forest/prefabs.bin");

// Spawn entities defined in the area
registry.SpawnEntity("ForestTree");
registry.SpawnEntity("ForestRock");
```

## API Reference

### RuntimePrefab

| Method | Description |
|--------|-------------|
| `RuntimePrefab(string name)` | Construct with a unique prefab name |
| `AddComponent(string typeName, unordered_map<string, string>)` | Add a component descriptor |
| `RemoveComponent(const string& typeName) -> bool` | Remove first matching component |
| `HasComponent(const string& typeName) -> bool` | Check if component type exists |
| `GetComponents() -> const vector<PrefabComponentData>&` | Get all component descriptors |
| `GetName() -> string_view` | Get the prefab name |
| `SetParent(const RuntimePrefab*)` | Set parent for inheritance (non-owning) |
| `GetParent() -> const RuntimePrefab*` | Get parent prefab or nullptr |
| `Clone() -> unique_ptr<RuntimePrefab>` | Deep copy with same name, components, parent |
| `Serialize(BinaryWriter&)` | Write to binary stream |
| `Deserialize(BinaryReader&)` | Read from binary stream |

### PrefabRegistry (Singleton)

| Method | Description |
|--------|-------------|
| `GetInstance() -> PrefabRegistry&` | Access the singleton |
| `RegisterPrefab(unique_ptr<RuntimePrefab>) -> uint32_t` | Register and take ownership, returns auto-assigned ID |
| `GetPrefab(uint32_t id) -> const RuntimePrefab*` | Look up by numeric ID |
| `GetPrefab(string_view name) -> const RuntimePrefab*` | Look up by name |
| `RemovePrefab(uint32_t id) -> bool` | Remove a prefab by ID |
| `SpawnEntity(uint32_t prefabId) -> uint32_t` | Spawn entity from prefab ID |
| `SpawnEntity(string_view name) -> uint32_t` | Spawn entity from prefab name |
| `GetRegisteredCount() -> uint32_t` | Number of registered prefabs |
| `GetAllPrefabNames() -> vector<string_view>` | Names of all registered prefabs |
| `SaveToFile(string_view path)` | Save all prefabs to binary file |
| `LoadFromFile(string_view path)` | Load and register prefabs from file |
| `Console_GetStatus() -> string` | Human-readable status for console |

## Thread Safety

- `PrefabRegistry` is a singleton with **no internal synchronization**. All registration, lookup, and spawn calls must happen on a single thread (typically the main thread).
- `RuntimePrefab` instances are value-like objects. A prefab obtained via `GetPrefab()` returns a `const` pointer and is safe to read from multiple threads as long as the registry is not being modified concurrently.
- `GetInstance()` uses a function-local static and is safe for concurrent first-access under C++11 magic-statics guarantees.

## See Also

- [[ECS-Overview]] -- Entity Component System architecture
- [[Components]] -- Available ECS component types
- [[Serializer]] -- Binary serialization utilities (BinaryWriter / BinaryReader)
- [[Scene-Management]] -- Scene loading and entity lifecycle
- [[Streaming-System]] -- Seamless area streaming and prefab loading
