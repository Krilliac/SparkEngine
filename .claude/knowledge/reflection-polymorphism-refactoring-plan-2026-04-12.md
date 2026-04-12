# Reflection & Polymorphism Refactoring — Full Project Plan

## Progress

| Phase | Status | Notes |
|-------|--------|-------|
| 1 (Inspector) | **Done** | FieldInfo extended (tooltip/category/isAssetPath/replicated/serialized/enumNames), SPARK_REFLECT_FIELD_ATTR macros, RenderReflectedFields with category/tooltip/enum support, 10 renderers migrated |
| 2 (Serialization) | **Foundation done** | ReflectionSerializer.h created (SerializeToProperties/DeserializeFromProperties/binary), not yet wired into JSONSceneSerializer |
| 3 (Save/Load) | Planned | RegisterReflectedSerializers already works, hand-written serializers still exist |
| 4 (Network) | Planned | |
| 5 (Materials) | Planned | |
| 6 (AngelScript) | Planned | |
| 7 (Settings) | Planned | |
| 8B (UI Bindings) | **Done** | UITypedBinding\<T\> replaces 4 concrete classes, backward-compatible aliases |

## Context

SparkEngine already has a **solid reflection foundation** in `Core/Reflection.h` (~530 lines) with `TypeRegistry`, `TypeInfo`, `FieldInfo`, `ComponentFactory`, and `SPARK_REFLECT_*` macros. 40+ components are registered. The reflection system now covers ~50% of inspector rendering (up from ~30%). The new `ReflectionSerializer.h` provides generic serialize/deserialize utilities. The remaining phases extend coverage further.

**Goal**: Eliminate manual per-type boilerplate across 8 subsystems by expanding the existing reflection system to ~95%. No external library — extend `Reflection.h` and `ComponentReflection.cpp`.

---

## Current State (What Already Exists)

| Component | File | Status |
|-----------|------|--------|
| `TypeRegistry` singleton | `Core/Reflection.h:60-150` | Production, 40+ types |
| `TypeInfo` (name, size, alignment, base, fields) | `Core/Reflection.h:25-58` | Complete |
| `FieldInfo` (name, type, offset, size, range, readOnly) | `Core/Reflection.h:10-24` | Complete but needs attributes |
| `ComponentFactory` (add/has/remove by string) | `Core/Reflection.h:284-373` | Production, 40+ components |
| `SPARK_REFLECT_TYPE/FIELD/END` macros | `Core/Reflection.h:378-451` | Production |
| `SetFieldFromString/GetFieldAsString` | `ComponentReflection.cpp` | Production, handles Vector3/4/float/int/bool/string/enum |
| `RegisterReflectedSerializers()` | `ComponentReflection.cpp` | Started — auto-generates save/load for reflected types |
| `RenderReflectedFields()` | `InspectorComponentRenderers_Reflected.cpp` | Production for ~13 components (volumes, probes) |
| `CheckedCast<To>(From)` | `Core/SafeCast.h` | Production (static_cast in Release, dynamic_cast in Debug) |
| `EventBus` type dispatch | `Utils/EventBus.h` | Uses `std::type_index` for channels |
| EnTT compile-time component IDs | `Engine/ECS/` | Deep integration, no manual type IDs needed |

---

## What Needs Refactoring — 8 Subsystems, Ranked by Impact

### Phase 1: Inspector Unification (HIGH impact, ~80% code reduction)
**Problem**: 70% of components still use manual `Render*Component()` functions in `InspectorComponentRenderers_*.cpp` — ~1000+ lines of near-identical ImGui boilerplate. Only ~13 components use `RenderReflectedFields()`.

**Solution**:
1. Ensure ALL 40+ components have `SPARK_REFLECT_TYPE` registrations in `ComponentReflection.cpp`
2. Add `FieldInfo` attributes: `tooltip`, `category` (string), `isAssetPath` (bool), `enumNames` (vector)
3. Extend `RenderReflectedFields()` to handle asset pickers (texture/mesh/material browsers) via `isAssetPath`
4. Replace each manual `Render*Component()` with a one-line call to `RenderReflectedFields()`
5. Wire reflected field edits through `CommandHistory` for automatic undo/redo

**Files to modify**:
- `Core/Reflection.h` — add `FieldInfo::tooltip`, `category`, `isAssetPath`, `enumNames`
- `Core/ComponentReflection.cpp` — complete registration for remaining components
- `SparkEditor/Source/Panels/InspectorComponentRenderers_*.cpp` — replace manual functions
- `SparkEditor/Source/CommandHistory.h` — add `ReflectedPropertyCommand` generic command

**Estimated reduction**: ~800 lines of manual ImGui code eliminated

---

### Phase 2: Scene Serialization Unification (HIGH impact, ~70% code reduction)
**Problem**: `SceneSerializer` has BOTH `BinarySceneSerializer.cpp` AND `JSONSceneSerializer.cpp`, each with per-component `switch(ComponentType)` cases. Adding a new component requires updating both serializers manually.

**Solution**:
1. Create `ReflectionSerializer` that iterates `TypeInfo.fields` to serialize/deserialize any reflected type
2. Binary format: field index + type tag + raw bytes (from `FieldInfo::offset` + `FieldInfo::size`)
3. JSON format: field name + type-appropriate JSON value (from `GetFieldAsString()`)
4. Both serializers share the same `TypeRegistry` iteration — no per-type switch cases
5. Version tag per-type: `TypeInfo` gets a `version` field; old saves trigger migration callbacks

**Files to modify**:
- `Core/Reflection.h` — add `TypeInfo::version`, optional `MigrateField` callback
- `SparkEditor/Source/SceneSystem/JSONSceneSerializer.cpp` — replace switch cases
- `SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp` — replace switch cases
- New: `Core/ReflectionSerializer.h/cpp` — generic type→JSON/binary conversion

**Estimated reduction**: ~2000 lines of duplicate serializer code

---

### Phase 3: Save/Load System (HIGH impact, eliminate manual lambdas)
**Problem**: `SaveSystem` requires per-component `Register("TypeName", serializeLambda, deserializeLambda)` calls. `RegisterReflectedSerializers()` was started but only covers reflected types.

**Solution**:
1. Complete `RegisterReflectedSerializers()` to cover ALL 40+ reflected components
2. Deprecate manual `ComponentSerializerRegistry::Register()` calls for reflected types
3. Use `ReflectionSerializer` (from Phase 2) as the backend
4. Keep manual registration ONLY for types with custom migration logic

**Files to modify**:
- `Engine/SaveSystem/SaveSystem.cpp` — remove manual lambda registrations, call `RegisterReflectedSerializers()`
- `Core/ComponentReflection.cpp` — ensure all components registered

**Estimated reduction**: ~300 lines of manual serialize/deserialize lambdas

---

### Phase 4: Network Replication (HIGH impact, correctness-critical)
**Problem**: `EntityReplicator` uses manual field indices (0-63 bitmask) with hand-written serialize/deserialize callbacks. Off-by-one errors in field indices cause silent data corruption.

**Solution**:
1. Auto-assign field indices from `TypeInfo.fields` iteration order (deterministic)
2. Generate serialize/deserialize from `FieldInfo` (type + offset) — no callbacks needed
3. Validate field count ≤ 64 at registration time (static_assert or runtime error)
4. Add `FieldInfo::replicated` attribute — only replicated fields get indices
5. Dirty-bit marking: `SetFieldFromString()` auto-marks the field's bit in the entity's dirty mask

**Files to modify**:
- `Core/Reflection.h` — add `FieldInfo::replicated` bool
- `Engine/Networking/EntityReplicator.h/cpp` — replace manual indices with reflection-driven
- `Engine/Networking/ReplicationFields.h` — simplify to use TypeRegistry
- `Engine/Networking/DeltaSnapshotManager.h` — use reflection for field comparison

**Estimated reduction**: ~400 lines; more importantly, eliminates a class of silent bugs

---

### Phase 5: Material Serialization (MEDIUM impact, ~90% code reduction)
**Problem**: `Material::SaveToFile/LoadFromFile` in `PBRMaterialLighting.cpp` manually writes/parses ~30 properties as INI key-value pairs (~150 lines each direction).

**Solution**:
1. Register `PBRProperties`, `AdvancedProperties`, `RenderState` as reflected types
2. Use `ReflectionSerializer` to auto-generate INI sections from `TypeInfo.fields`
3. Section names from type names, key names from `FieldInfo::fieldName`
4. Keep [Textures] and [Variants] sections manual (they're maps, not flat structs)

**Files to modify**:
- `Core/ComponentReflection.cpp` — add `SPARK_REFLECT_TYPE(PBRProperties)` etc.
- `Graphics/PBRMaterialLighting.cpp` — replace SaveToFile/LoadFromFile with reflection loop
- `Graphics/PBRMaterialLightingWindows.cpp` — same for LoadFromFile

**Estimated reduction**: ~250 lines

---

### Phase 6: AngelScript Auto-Binding (MEDIUM impact)
**Problem**: 60+ engine functions manually registered with AngelScript. Scripts can't access custom component properties generically.

**Solution**:
1. Iterate `TypeRegistry` at script engine startup to auto-register all reflected types
2. For each `TypeInfo`: register type as value type; for each `FieldInfo`: register getter/setter
3. Add generic `GetField(EntityID, "Component", "field")` / `SetField(...)` script functions
4. Keep manual bindings for complex APIs (math, physics queries, input) where auto-binding doesn't fit

**Files to modify**:
- `Engine/Scripting/AngelScriptEngine.cpp` — add `AutoRegisterReflectedTypes()` at init
- New: `Engine/Scripting/ScriptReflectionBridge.h/cpp` — reflection→AngelScript bridge

**Estimated reduction**: ~200 lines of manual registration; enables scripts to access any reflected field

---

### Phase 7: Settings Auto-Serialization (MEDIUM impact)
**Problem**: `EngineSettings` manually reads/writes each config field from INI.

**Solution**:
1. Register `GraphicsSettings`, `AudioSettings`, `PhysicsSettings` etc. as reflected types
2. Generic `LoadSettingsFromConfig(TypeInfo*, void* settings, ConfigParser&)` reads all fields
3. Generic `WriteSettingsToConfig(...)` writes all fields

**Files to modify**:
- `Core/EngineSettings.cpp` — replace manual field reads with reflection loop
- `Core/ComponentReflection.cpp` — register settings structs

**Estimated reduction**: ~200 lines

---

### Phase 8: Polymorphism Cleanup (LOW-MEDIUM impact, architectural hygiene)

#### 8A. RHI Interface Segregation
`IRHIDevice` has ~25 methods. Split into focused interfaces:
- `IRHIResourceFactory` — CreateBuffer, CreateTexture, CreateShader, CreateSampler
- `IRHIFrameManager` — BeginFrame, EndFrame, Present, WaitForIdle
- `IRHIStateManager` — SetViewport, SetScissor, GetBackBufferFormat
- Each backend (D3D11, Vulkan, OpenGL, Null) implements all three via multiple inheritance

**Files**: `Graphics/RHI/RHIDevice.h`, all backend `*Device.h` files

#### 8B. UI Data Binding Templatization
Replace `UIFloatBinding`, `UIStringBinding`, `UIBoolBinding`, `UIIntBinding` with `UIDataBinding<T>`.

**Files**: `Engine/UI/UISystem.h`

#### 8C. Physics Backend Integration
Connect `IPhysicsBackend` into `EngineContext`. Make `PhysicsSystem` inherit `IPhysicsBackend`.

**Files**: `Physics/PhysicsSystem.h`, `Core/EngineContext.h`

#### 8D. Editor Panel Categorization (optional)
Add lightweight `EditorViewportPanel`, `EditorToolPanel` intermediate classes for the 55+ panels.

**Files**: `SparkEditor/Source/Core/EditorPanel.h`

---

## New FieldInfo Attributes (Phase 1 prerequisite)

```cpp
struct FieldInfo
{
    // Existing:
    std::string name;          // Display name ("Position")
    std::string fieldName;     // C++ member name ("position")
    FieldType type;            // Float, Int, Bool, String, Vector3, Vector4, Enum, ...
    size_t offset;
    size_t size;
    TypeId ownerType;
    float rangeMin, rangeMax;
    bool hasRange = false;

    // New attributes for reflection refactoring:
    std::string tooltip;                    // Editor hover text
    std::string category;                   // Group in Inspector ("Rendering", "Physics")
    bool isAssetPath = false;               // Shows asset picker in Inspector
    bool replicated = false;                // Included in network replication
    bool readOnly = false;                  // Cannot be edited in Inspector
    bool serialized = true;                 // Included in save/load (default true)
    std::vector<std::string> enumNames;     // Enum value names for dropdown display
};
```

New macro:
```cpp
#define SPARK_REFLECT_FIELD_ATTR(Type, member, displayName, ...) \
    // Variadic attribute setting: tooltip("..."), category("..."), replicated(true), etc.
```

---

## Implementation Order & Dependencies

```
Phase 1 (Inspector)
  └─ Requires: FieldInfo attributes, complete component registration
  
Phase 2 (Scene Serialization)
  └─ Requires: ReflectionSerializer (new), Phase 1 registration
  
Phase 3 (Save/Load)
  └─ Requires: Phase 2 ReflectionSerializer
  
Phase 4 (Network Replication)
  └─ Requires: FieldInfo::replicated attribute from Phase 1
  
Phase 5 (Materials)
  └─ Requires: Phase 2 ReflectionSerializer
  
Phase 6 (AngelScript)
  └─ Requires: Phase 1 complete registration
  
Phase 7 (Settings)
  └─ Requires: Phase 2 ReflectionSerializer
  
Phase 8 (Polymorphism)
  └─ Independent of Phases 1-7
```

Phases 1-3 form the critical path. Phases 4-7 can be done in any order after Phase 2. Phase 8 is independent.

---

## Verification Strategy

After each phase:
1. `cmake --build build/linux-gcc-release --config Release` — zero errors
2. `cd build/linux-gcc-release && ctest --output-on-failure` — all 5566+ tests pass
3. Manually verify in editor (if graphical): Inspector renders all components, scene save/load round-trips
4. For Phase 4: network replication smoke test (if available)

New tests to add per phase:
- Phase 1: `TestReflectionInspector.cpp` — verify all 40+ components render without crashes
- Phase 2: `TestReflectionSerializer.cpp` — round-trip serialize/deserialize all reflected types
- Phase 3: Extend `TestSaveSystem*.cpp` — verify reflected serializers produce correct output
- Phase 4: `TestReflectedReplication.cpp` — field index assignment, dirty bit tracking
- Phase 8: `TestRHIInterfaceSegregation.cpp` — verify split interfaces compile and link

---

## Estimated Scope

| Phase | Files Modified | Files Created | Lines Reduced | Lines Added | Net |
|-------|---------------|---------------|---------------|-------------|-----|
| 1 | ~8 | 0 | ~800 | ~200 | -600 |
| 2 | ~4 | 1 | ~2000 | ~300 | -1700 |
| 3 | ~2 | 0 | ~300 | ~50 | -250 |
| 4 | ~4 | 0 | ~400 | ~150 | -250 |
| 5 | ~3 | 0 | ~250 | ~50 | -200 |
| 6 | ~2 | 1 | ~200 | ~150 | -50 |
| 7 | ~2 | 0 | ~200 | ~50 | -150 |
| 8 | ~10 | ~3 | ~100 | ~200 | +100 |
| **Total** | **~35** | **~5** | **~4250** | **~1150** | **-3100** |

Net result: ~3100 fewer lines of manual boilerplate, zero new external dependencies, full backward compatibility.

---

## Key Files Reference

| File | Role | Lines |
|------|------|-------|
| `Core/Reflection.h` | TypeRegistry, FieldInfo, macros | 452 |
| `Core/ComponentReflection.cpp` | 40+ component registrations | 600 |
| `Core/SafeCast.h` | CheckedCast (debug dynamic_cast) | 93 |
| `Utils/EventBus.h` | Type-erased publish/subscribe | 338 |
| `Engine/SaveSystem/SaveSystem.cpp` | Save/load with manual lambdas | ~400 |
| `SparkEditor/Source/SceneSystem/JSONSceneSerializer.cpp` | Per-type JSON switch | ~500 |
| `SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp` | Per-type binary switch | ~500 |
| `SparkEditor/Source/Panels/InspectorComponentRenderers_*.cpp` | Manual per-component UI | ~1200 |
| `Engine/Networking/EntityReplicator.h` | Manual field indices | ~300 |
| `Engine/Scripting/AngelScriptEngine.cpp` | Manual script binding | ~400 |
| `Graphics/PBRMaterialLighting.cpp` | Manual INI I/O | 267 |
| `Core/EngineSettings.cpp` | Manual config parsing | ~400 |
| `Graphics/RHI/RHIDevice.h` | 25-method monolithic interface | ~300 |
