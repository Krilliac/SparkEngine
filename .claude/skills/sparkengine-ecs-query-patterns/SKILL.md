---
name: sparkengine-ecs-query-patterns
description: How to query and iterate the EnTT-based ECS in SparkEngine — building views over component combinations, iterating entities inside a system, the CoreComponents vs domain-header layout, the data-driven archetype/prefab spawn format, and the fixed Physics->Animation->AI->Audio->Lifecycle->Render execution order. TRIGGER when the user says "iterate entities with component X", "write a new ECS system", "loop over all entities that have", "GetEntitiesWith", "registry.view", "EnTT view or group", "how do I spawn a prefab/archetype", "what order do systems run in", "why does my system read a stale Transform", or "where do I add a new component struct". DO NOT TRIGGER for how to REGISTER a component with reflection/editor/save (see sparkengine-reflection-conventions), for AngelScript scripting (see sparkengine-hot-reload-seam), or for generic EnTT questions unrelated to this codebase's conventions.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine ECS query patterns

SparkEngine's ECS is a thin wrapper over **EnTT** (`entt::registry`). This skill
covers how entities and components are *queried and iterated* in this codebase —
the real idioms used in the shipped systems, not invented EnTT examples. It does
**not** cover reflection/editor/save registration of component types: for that,
read the sibling skill **sparkengine-reflection-conventions** first ("how do I
register a new component type", editor Inspector, `SPARK_REFLECT_*`).

Jargon, defined once:
- **Entity** — an opaque ID (`entt::entity`, aliased `EntityID`). Just a number.
- **Component** — a plain data struct (`Transform`, `RigidBodyComponent`, …).
- **System** — a logic class (`: public ISystem`) that iterates entities holding
  a specific component combination and updates them each frame.
- **View** — an EnTT read/write cursor over every entity that has *all* of a given
  set of components. Cheap to construct; build it fresh each frame, never cache it.
- **World** — `class World` (in `Components.h`), the thin owner of one
  `entt::registry`. Every Scene owns exactly one `World`.

## The World API you actually call

`World` (defined in `SparkEngine/Source/Engine/ECS/Components.h`, global scope)
wraps the registry. Prefer these over touching `entt::registry` directly — they
add `SPARK_REQUIRE` validity assertions:

| Call | What it does |
| --- | --- |
| `world.CreateEntity("name")` | Create an entity; if a name is given, also emplaces a `NameComponent`. |
| `world.DestroyEntity(e)` | Validity-checked destroy; also unsubscribes the entity from `EntityEventBus`. |
| `world.AddComponent<T>(e, args...)` | `emplace<T>`. **Asserts the entity does NOT already have `T`** — adding twice is a hard error, not an overwrite. |
| `world.GetComponent<T>(e)` | Returns `T*` (or `const T*`), `nullptr` if absent. This is `registry.try_get<T>` — never dereference without a null check. |
| `world.HasComponent<T>(e)` | `all_of<T>`. |
| `world.RemoveComponent<T>(e)` | Asserts the component is present first. |
| `world.GetEntitiesWith<A, B, ...>()` | Returns `registry.view<A, B, ...>()` — the primary query entry point. |
| `world.GetRegistry()` | Escape hatch to the raw `entt::registry&` for calls the wrapper doesn't expose (e.g. `try_get` a *third* component mid-loop, single-component views). |

## Pattern 1 — the standard per-system iteration (use this by default)

Every gameplay/render/physics system in `Systems/ECSystems.cpp` and `Systems/Systems2D.h`
follows the same shape. Build the view, `for (auto entity : view)`, then pull each
component with `view.get<T>(entity)` (returns a reference — no null check needed,
the view guarantees presence). Verbatim from `RenderSystem::Update`
(`ECSystems.cpp`):

```cpp
const auto& registry = world.GetRegistry();
auto view = world.GetEntitiesWith<Transform, MeshRenderer>();
for (auto entity : view)
{
    auto& transform = view.get<Transform>(entity);
    auto& renderer  = view.get<MeshRenderer>(entity);

    if (!renderer.visible)
        continue;

    // Optional third component that is NOT part of the view filter:
    // use the registry's try_get, because the view doesn't hold it.
    auto* active = registry.try_get<ActiveComponent>(entity);
    if (active && !active->active)
        continue;

    XMMATRIX worldMtx = transform.GetWorldMatrix(registry);
    XMStoreFloat4x4(&renderer.cachedWorldMatrix, worldMtx);
    renderer.worldMatrixDirty = false;
}
```

Rules this codebase follows:
- **The view filters on the type list; every listed type is guaranteed present**,
  so `view.get<T>(entity)` returns a live reference. Reach for `registry.try_get<T>`
  only for an *optional* extra component you did not put in the view type list (like
  `ActiveComponent` above) — that avoids a second hash lookup through
  `World::GetComponent`.
- **Construct the view fresh at the top of `Update()`.** Views are lightweight
  cursors; they are never stored across frames anywhere in this codebase.
- **Never hold component references across frames.** `ISystem::Update` gets `World&`
  each call for exactly this reason (see `ECSystemTypes.h`).

## Pattern 2 — structured-binding `.each()` iteration

EnTT views also expose `.each()`, which yields `[entity, comp, comp...]` tuples so
you skip the separate `view.get<>` calls. Used in `Systems/FixedTimestepPhysics.h`
(`StepPhysics`):

```cpp
auto view = world.GetEntitiesWith<RigidBodyComponent, Transform>();
for (auto [entity, rb, tf] : view.each())
{
    if (rb.type == RigidBodyComponent::Type::Kinematic)
    {
        rb.previousPosition = tf.position;
        rb.previousRotation = tf.rotation;
    }
}
```

Both styles are correct and both appear in the tree. Pick `.each()` when you use
every component in the type list; pick the `for (auto entity : view)` +
`view.get<>` style when you need the raw `entity` id often (e.g. for logging,
`static_cast<uint32_t>(entity)`, or passing to physics/network APIs) or when you
conditionally touch only some components.

## Pattern 3 — single-component view via the raw registry

For a query on exactly one component, `AbilitySystem::Update` (`ECSystems.cpp`)
goes straight through the registry rather than `GetEntitiesWith`:

```cpp
auto view = world.GetRegistry().view<AbilityComponent>();
for (auto entity : view)
{
    auto& ability = view.get<AbilityComponent>(entity);
    ability.Update(deltaTime);
}
```

`world.GetEntitiesWith<AbilityComponent>()` is equivalent and also valid — both
forward to `registry.view<...>()`. There is no wrapper difference for the query
itself; use whichever reads clearer.

## Groups — not used here (do not add them casually)

EnTT `registry.group<...>()` (owning/partial-owning groups for tighter iteration)
is **not** used anywhere in the ECS layer as of this writing — every query is a
plain `view`. Groups take ownership of component storage layout and can conflict
with other groups/views over the same types; do not introduce one to "optimize" a
single system without a measured need and a check that no other system iterates the
same component set. Views are the house style.

## Component header organization

Component structs are **plain data** and live under
`SparkEngine/Source/Engine/ECS/Components/`, split by domain for compile speed. Do
NOT put logic (methods beyond trivial helpers) in them and do NOT scatter
reflection blocks next to them (reflection lives in one file — see the sibling
skill).

- **`Components/CoreComponents.h`** — the always-present core: `NameComponent`,
  `Transform`, `MeshRenderer`, `Camera`, `Script`. Plus the `using EntityID =
  entt::entity;` alias. Touch this only for genuinely universal components.
- **Domain headers** (one per subsystem): `PhysicsComponents.h`,
  `AudioComponents.h`, `LightComponents.h`, `AnimationComponents.h`,
  `AIComponents.h`, `NetworkComponents.h`, `GameplayComponents.h`,
  `Sprite2DComponents.h`, `FPSComponents.h`, `SplineComponents.h`,
  `VolumeComponents.h`, `PlacementComponents.h`, `AdvancedPlacementComponents.h`,
  `CollisionMaskComponents.h`, `VisibilityComponents.h`, `TerrainComponents.h`.
- **`Engine/ECS/Components.h`** — the umbrella header. It `#include`s every domain
  header **and** defines `class World`. Include the specific domain header you need
  in a system for faster compiles; include the umbrella `Components.h` only when you
  need `World` itself or many component families at once (its own header comment
  says to prefer the narrow includes).

New component → put the struct in the closest domain header (or start a new
`NewDomainComponents.h` for a new subsystem), then follow **sparkengine-reflection-conventions**
to register it. Adding the struct alone is enough to `view`/`GetEntitiesWith` it;
reflection is only needed for editor/save/archetype-by-string-name.

## Archetype (prefab) loading — data-driven spawning

`Engine/ECS/EntityArchetypeLoader.cpp` loads text archetype files and spawns
entities from them, bridging to components by **string name** through the
reflection `ComponentFactory`. Two data structures back it (`EntityArchetype.h`):
`Archetype { name, category, vector<ComponentEntry> }` and
`ComponentEntry { typeName, unordered_map<string,string> properties }`, held in the
`EntityArchetypeSystem` singleton keyed by name.

### File format (parsed by `LoadArchetypeFromFile`)

Line-based `key = value`. `//` lines and blanks are skipped. Recognized keys:

| Key | Meaning |
| --- | --- |
| `name` | Archetype name (required; a file with no `name` is rejected). |
| `category` | Free-form grouping string (e.g. `Characters`, `Props`). |
| `component` | One component line. Repeat for each component. |

A `component` value is `TypeName: p0 / p1 / p2 ...` — the part before `:` is the
component type name; the part after is split on `/` into **positional** parameters
stored as properties `"p0"`, `"p1"`, …. A bare `TypeName` with no `:` adds the
component with no properties. Example:

```
name = GuardNPC
category = Characters
// position / rotation / scale, each as "x,y,z"
component = Transform: 0,0,0 / 0,90,0 / 1,1,1
component = LightComponent: Point / 1,0.9,0.7 / 4.0 / 12.0
component = MeshRenderer
```

### Spawn flow (`SpawnFromArchetype`)

`EntityID SpawnFromArchetype(const std::string& name, World& world)`:
1. Look up the `Archetype` by name in `EntityArchetypeSystem::GetInstance()`;
   return `entt::null` and warn if missing.
2. `world.CreateEntity(archetype->name)` — the entity is named after the archetype.
3. For each `ComponentEntry`, call `ApplyComponentViaReflection`.

`ApplyComponentViaReflection` picks one of three paths per component:
- **`NameComponent`** → special-cased (`CreateEntity` may have already added one, so
  it only adds when absent).
- **Positional-param legacy path** — if any property key is `p<digit>` AND the type
  is `Transform` or `LightComponent`, a hand-written `ApplyTransform` /
  `ApplyLightComponent` parses the compact `"x,y,z / r,g,b / ..."` syntax. This
  exists because those compact vectors don't map to reflected field *names*.
- **Reflection path** (everything else) — requires the type to be registered in the
  reflection `ComponentFactory`:
  `factory.AddComponent(typeName, &world, entityId)`, then for each property look up
  the field on `TypeRegistry::Get().FindTypeByName(typeName)` and
  `SetFieldFromString`. **If the type isn't registered with `ComponentFactory`, the
  component is silently skipped with a warning** — this is the exact gap that step 3
  of the reflection skill's checklist prevents.

There is an override overload:
`SpawnFromArchetype(name, world, unordered_map<string,string> overrides)`. Override
keys use `"ComponentType.propertyName"` (dot-separated); matching properties on the
copied component entries are replaced before spawning. Keys without a `.` are
ignored.

## Execution order invariant

The fixed ECS order (from `D:\SparkEngine\CLAUDE.md`) is:

```
Physics -> Animation -> AI -> Audio -> Lifecycle -> Render
```

Rationale, per the system doc comments in `ECSystems.h`: Physics writes `Transform`;
Animation produces bone matrices; AI *reads* `Transform` and writes velocity/target;
Audio reads `Transform` to position 3D sources; Lifecycle handles health/death/active;
Render reads `Transform` + `MeshRenderer` last so it draws the settled frame. Getting
this wrong produces a one-frame lag / stale-transform read (e.g. AI steering off last
frame's position), not a crash — which is why it's easy to miss.

**Where the order is actually enforced — know which manager you have:**

- **`SystemManager`** (`ECSystems.h`) is a **flat list, insertion-order** manager.
  `AddSystem<T>()` does `push_back`; `UpdateAll()` iterates in insertion order with
  **no sorting**. The order is therefore enforced *by the caller registering systems
  in the correct order* — it is a convention, not an automatic guarantee. The header's
  `## System execution order` comment documents the required sequence and its `@code`
  block shows the correct registration order.
- **`PhaseSystemManager`** (`Systems/PhaseSystemManager.h`) is the deterministic
  alternative: `AddSystem<T>(Phase, ...)` buckets systems by a `Phase` enum
  (`PrePhysics, Physics, PostPhysics, Animation, AI, Audio, Gameplay, PreRender,
  Render, PostRender`) and `UpdateAll()` runs phase buckets **in enum order
  regardless of insertion order** (unphased systems run last). It is the *intended*
  structural home. Note its comment records that an earlier parallel-execution path
  was **removed** because it batched across all phases and silently broke phase
  ordering — it runs serially today.

> **Wiring reality — do not assume any of these ticks (verified 2026-07-07).** None of
> these managers is cleanly wired into the shipping loop right now, so registering a
> system into one does **not** guarantee it runs. `PhaseSystemManager` is
> **defined-not-wired** (`CreatePhaseSystemManager` has zero callers, `UpdateAll` is
> never invoked); a third class, `StageBasedExecutor` (`ParallelSystemExecutor.h`),
> **is** ticked every frame (`GameplayLifecycleShared.cpp:1062`) but has zero registered
> systems (dead tick); the shipping Core loop drives module systems via
> `moduleManager->UpdateAll(dt)`. This is a known OPEN architectural weak point — if you
> add a system, verify the tick path end-to-end and flag the gap. See
> `sparkengine-architecture-contract` Invariant 3 and `sparkengine-job-system-threading §1c`.

**`SplineFollowerSystem`** slots between Animation and AI:
`Physics -> Animation -> SplineFollower -> AI -> Audio -> Lifecycle -> Render`
(so spline-driven entities are positioned before AI reads their transforms).

### Gotcha: the ordering unit test is a standalone mock

`Tests/TestECSystemOrdering.cpp` asserts that adding systems in the *wrong* order
still executes Physics->…->Render, i.e. it "sorts by priority". That test is a
**standalone reimplementation** (its own file header says: "mirrors
Spark::ECS::SystemManager and ISystem without engine dependencies for CI/Linux
compatibility") — its auto-sorting behavior is the *mock's*, **not** the real
`SystemManager`, which does not sort. Do not rely on the real flat `SystemManager`
to reorder for you; register in order, or use `PhaseSystemManager`.

## When NOT to use this skill

- Registering a component for the editor/save/network/archetype-by-name →
  **sparkengine-reflection-conventions**.
- AngelScript-side entity access / hot reload → **sparkengine-hot-reload-seam**.
- Scene file save/load format → **sparkengine-serialization-format**.

## Provenance and maintenance

Verified by Read/Grep against these files on **2026-07-07** (branch
`claude/forge-sonnet5-skills` @ `9fb17e63`):
`Engine/ECS/Components.h`, `Engine/ECS/Components/CoreComponents.h`,
`Engine/ECS/EntityArchetype.h`, `Engine/ECS/EntityArchetypeLoader.cpp`,
`Engine/ECS/Systems/ECSystemTypes.h`, `Engine/ECS/Systems/ECSystems.h`,
`Engine/ECS/Systems/ECSystems.cpp`, `Engine/ECS/Systems/Systems2D.h`,
`Engine/ECS/Systems/FixedTimestepPhysics.h`,
`Engine/ECS/Systems/PhaseSystemManager.h`, `Tests/TestECSystemOrdering.cpp`.

Re-verify commands (run from repo root):
- View/query idioms still current:
  `grep -rn "GetEntitiesWith\|\.each()\|GetRegistry().view" SparkEngine/Source/Engine/ECS`
- Groups still unused (expect no ECS-layer hits):
  `grep -rn "registry.group<\|\.group<" SparkEngine/Source/Engine/ECS`
- Flat SystemManager still has no sort (should show only push_back / ordered iterate):
  `grep -n "std::sort\|stable_sort\|push_back" SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`
- Domain component headers inventory:
  `ls SparkEngine/Source/Engine/ECS/Components/`
- Archetype format keys unchanged:
  `grep -n "key ==" SparkEngine/Source/Engine/ECS/EntityArchetypeLoader.cpp`
- Validate this skill with the skill-linter (see the global skill-linter tool under `~/.claude/skills/skill-linter`).
