---
name: sparkengine-reflection-conventions
description: How to register a new reflected ECS component type in SparkEngine's hand-rolled, no-RTTI reflection system (Core/Reflection.h + Core/ComponentReflection.cpp). TRIGGER when the user says "add a reflected field", "expose this component to the editor inspector", "why isn't my component showing up in the Inspector", "add SPARK_REFLECT", "make this field replicated/serialized", "register a new component type", "TypeRegistry", "ComponentFactory", or "reflection macro". DO NOT TRIGGER for AngelScript scripting reflection (see sparkengine-hot-reload-seam), for scene-file save/load mechanics beyond the reflected-field bridge (see sparkengine-serialization-format), or for generic C++ RTTI/dynamic_cast questions unrelated to this system.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine reflection conventions

## What this system is (and isn't)

`SparkEngine/Source/Core/Reflection.h` is a **lightweight, compile-time, no-RTTI**
type reflection system (ezEngine-inspired), used for: editor Inspector rendering,
scene save/load, network replication field marking, and data-driven archetype
(prefab) spawning. It does **not** use `dynamic_cast`/`typeid` — type identity is
the same `TypeId`/`GetTypeId<T>()` pattern used by `EngineContext` (defined in
`Core/EngineContext.h`, reused here rather than duplicated — a duplicate
definition causes an "already defined" error on MSVC).

There is exactly **one file** where every ECS component gets reflected:
`SparkEngine/Source/Core/ComponentReflection.cpp`. Reflection blocks are NOT
scattered next to each component's own header/cpp — this is a deliberate
single-file convention so `TypeRegistry`/`ComponentFactory` registration order
and coverage is auditable in one place.

## Core types (`Core/Reflection.h`)

| Type | Purpose |
| --- | --- |
| `Spark::FieldType` (enum) | `Bool, Int, Float, Double, String, Vector2, Vector3, Vector4, Enum, Custom` |
| `Spark::FieldInfo` | One field's metadata: name, offset (`offsetof`), size, type, plus editor/network/save attributes (below) |
| `Spark::TypeInfo` | One type's metadata: name, `TypeId`, size, alignment, `baseType`, schema `version`, `fields` vector |
| `Spark::TypeRegistry` | Global singleton; `RegisterType()`, `FindType(TypeId)`, `FindTypeByName()`, `GetAllTypes()`, `GetDerivedTypes<Base>()` |
| `Spark::ComponentFactory` | Global singleton; type-erased `add`/`has`/`remove`/`getRaw` callbacks keyed by component **name string** — bridges data-driven code (archetype files) to the compile-time `World::AddComponent<T>()` API |

`FieldInfo` extended attributes you can set per-field:

| Attribute | Meaning |
| --- | --- |
| `rangeMin`/`rangeMax`/`hasRange` | Editor slider clamp (set via `SPARK_REFLECT_FIELD_RANGE`) |
| `readOnly` | Inspector shows but doesn't allow editing |
| `tooltip` | Editor hover text |
| `category` | Inspector grouping (e.g. `"Physics"`, `"Rendering"`) |
| `isAssetPath` | Shows an asset picker instead of a text box |
| `replicated` | Included in network replication (networking code queries this) |
| `serialized` | Included in scene save/load — **defaults to `true`** |
| `enumNames` | Dropdown labels for `Enum` fields |
| `visibleWhenField`/`visibleWhenValue` | Conditional Inspector visibility — this field only renders when another named field equals a value |

**Thread safety** (stated in the header): registration happens at static-init
time (single-threaded, before `main`), both registries are read-only after
startup and safe to query from any thread.

## Registration macros

All defined at the bottom of `Reflection.h`, used together inside a per-type
block in `ComponentReflection.cpp`:

```cpp
SPARK_REFLECT_TYPE(Transform)
SPARK_REFLECT_FIELD_ATTR_AS(Transform, position, "Position", Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_FIELD_ATTR_AS(Transform, rotation, "Rotation", Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_FIELD_ATTR_AS(Transform, scale,    "Scale",    Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_END(Transform)
```

| Macro | When to use |
| --- | --- |
| `SPARK_REFLECT_TYPE(Type)` | Start a block. One .cpp registration per type (a static initializer struct is generated named `SparkReflect_<Type>`). |
| `SPARK_REFLECT_TYPE_BASE(Type, BaseType)` | Same, but also records `baseType` for `GetDerivedTypes<Base>()` queries. |
| `SPARK_REFLECT_FIELD(Type, member, "Display Name")` | Plain field; `FieldType` auto-deduced via `DeduceFieldType<T>()`. |
| `SPARK_REFLECT_FIELD_RANGE(Type, member, "Display Name", min, max)` | Adds an editor range clamp. |
| `SPARK_REFLECT_FIELD_ATTR(Type, member, "Display Name", <stmts>)` | Auto-deduced type + arbitrary `field.<attr> = ...;` statements (set `tooltip`/`category`/`replicated`/etc). |
| `SPARK_REFLECT_FIELD_ATTR_AS(Type, member, "Display Name", FieldType::X, <stmts>)` | Explicit `FieldType` + arbitrary attribute statements — **use this for DirectXMath vectors** (see gotcha below). |
| `SPARK_REFLECT_FIELD_AS(Type, member, "Display Name", FieldType::X)` | Explicit `FieldType`, no extra attributes. Defined locally in `ComponentReflection.cpp` (not `Reflection.h`) — same shape as `SPARK_REFLECT_FIELD` but skips auto-deduction. |
| `SPARK_REFLECT_FIELD_VISIBLE_WHEN(Type, member, "Display Name", controlMember, controlVal)` | Field only shows in Inspector when `controlMember == controlVal`. |
| `SPARK_REFLECT_VERSION(ver)` | Set `TypeInfo::version` (schema version, for save-file migration) — place before `SPARK_REFLECT_END`. |
| `SPARK_REFLECT_END(Type)` | Close the block. `Type` here must match the opening macro's argument. |

## Checklist: adding a new reflected component

1. Define the component struct in its own `Components*.h` header as usual (not
   in `ComponentReflection.cpp`).
2. In `SparkEngine/Source/Core/ComponentReflection.cpp`, add one
   `SPARK_REFLECT_TYPE(YourType) ... SPARK_REFLECT_END(YourType)` block,
   grouped under the matching `// --- Category ---` comment section (Core,
   Physics, Audio, Light, Animation, AI, Networking, Gameplay, FPS, Spline,
   Volumes, Terrain, 2D/Sprite, Placement, Advanced Placement, Collision Mask,
   Visibility — pick the closest match or start a new section).
3. Inside `ComponentFactoryRegistrar`'s constructor (same file, bottom
   `namespace { ... }` block), add `SPARK_REGISTER_COMPONENT(YourType)` in the
   matching category section. **Skipping this step means the type reflects
   fine (Inspector/save work) but data-driven archetype spawning
   (`EntityArchetypeLoader.cpp`) can't `AddComponent`/`HasComponent` it by
   string name** — a silent gap, not a compile error.
4. **DirectXMath vector fields (`XMFLOAT2`/`XMFLOAT3`/`XMFLOAT4`) always need
   explicit typing.** `DeduceFieldType<T>()` only recognizes `bool`,
   integral types, `float`, `double`, `std::string`, and `enum` — anything
   else (including `XMFLOAT3`) deduces to `FieldType::Custom` and silently
   won't serialize/display correctly. Use `SPARK_REFLECT_FIELD_AS` (or
   `_ATTR_AS` if you also need attributes) with the explicit
   `Spark::FieldType::Vector2/3/4`.
5. Set `field.replicated = true` for any field the network layer must sync
   (via `SPARK_REFLECT_FIELD_ATTR`/`_ATTR_AS`); set `field.serialized = false`
   to explicitly exclude a field from scene save/load (default is `true`,
   i.e. saved unless you opt out).
6. Build once — registration is compile-checked (macro references
   `offsetof(Type, member)`, so a typo'd member name fails to compile, not
   silently at runtime).

## `enum class` gotcha (real bug, now fixed — know the pattern)

`DeduceFieldType<T>()` maps any `enum` to `FieldType::Enum`, but a plain
`enum class` with no explicit underlying type has no reflectable storage type
at the ABI level — MSVC lays it out as `int`. `SetFieldFromString`/
`GetFieldAsString` in `ComponentReflection.cpp` therefore round-trip `Enum`
fields via `int` explicitly. A code comment at both call sites notes this
used to fall through to the `default:` case and **silently drop the write**
(scene reload would reset the field) — if you ever see an enum-typed
Inspector field that won't stick across a scene reload, this is the first
thing to check: is it going through the `FieldType::Enum` case, and is that
case actually doing the int round-trip, not falling through?

## Where reflected data gets consumed

- **Scene save/load**: `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp`
  — walks `TypeRegistry::Get().FindTypeByName(...)`, skips fields where
  `!f.serialized`.
- **Data-driven archetype/prefab spawning**: `SparkEngine/Source/Engine/ECS/EntityArchetypeLoader.cpp`
  — parses a text archetype file, resolves component type + field names via
  `TypeRegistry`, and adds components via `ComponentFactory` (string-keyed,
  not templated) — this is why step 3 of the checklist above matters.
- **Editor Inspector panel**: consumes `category`/`tooltip`/`isAssetPath`/
  `visibleWhenField`/`rangeMin`/`rangeMax` for rendering (panel code not
  covered by this skill — see the editor panel registry if extending
  Inspector rendering itself).

## Known stale-doc trap (candidate, unverified)

`ComponentReflection.cpp` has two section comments — `// --- Spline (missing
reflection) ---` (around `SplineComponent`) and `// --- Volumes (missing
reflection) ---` (around `LODGroupComponent`) — that read like leftover work markers,
but both types **are** fully reflected in the blocks directly below those
comments as of this writing. Treat the comment text as stale, not as a real
gap; if you're asked to "finish" Spline/LODGroup reflection, verify first
rather than assuming work remains.

## Provenance and maintenance

- Verified against `SparkEngine/Source/Core/Reflection.h`,
  `SparkEngine/Source/Core/ComponentReflection.cpp`,
  `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp`, and
  `SparkEngine/Source/Engine/ECS/EntityArchetypeLoader.cpp` on 2026-07-07
  (branch `claude/forge-sonnet5-skills`, based on `Working` @ `9fb17e63`).
- Re-verify component count / registration parity:
  `grep -c "SPARK_REFLECT_TYPE(" SparkEngine/Source/Core/ComponentReflection.cpp`
  vs. `grep -c "SPARK_REGISTER_COMPONENT(" SparkEngine/Source/Core/ComponentReflection.cpp`
  — these should track closely; a growing gap means new types are being
  reflected but not registered with `ComponentFactory` (step 3 above).
- Re-verify the stale-comment claim:
  `grep -n "missing reflection" SparkEngine/Source/Core/ComponentReflection.cpp`
