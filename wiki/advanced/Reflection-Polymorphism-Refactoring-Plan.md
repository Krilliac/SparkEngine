# Reflection & Polymorphism Refactoring Plan

> **Audience:** Programmers
>
> **Thread Context:** Reflection registration happens at startup (TypeRegistry, ComponentFactory). Field get/set runs on whichever thread calls it (inspector = main thread; replication = network thread). No special threading guarantees beyond the subsystems that consume reflection.
>
> **Platform/Backend Scope:** Cross-platform. The reflection core (`Core/Reflection.h`, `Core/ReflectionSerializer.h`) is pure CPU and compiles everywhere.

## Overview

SparkEngine has a solid hand-rolled reflection foundation in `Core/Reflection.h` (`TypeRegistry`, `TypeInfo`, `FieldInfo`, `ComponentFactory`, `SPARK_REFLECT_*` macros). The goal of this plan was to eliminate manual per-type boilerplate across eight subsystems — inspector rendering, scene serialization, save/load, network replication, materials, AngelScript binding, settings, and assorted polymorphism cleanup — by expanding reflection coverage toward ~95%, with no external library and full backward compatibility.

## Current Status (as of 2026-06-08)

**Overall: Substantially Completed. Most phases Done; a few Partial/Deferred items remain.**

| Phase | Status | Evidence |
|-------|--------|----------|
| 1 — Inspector | **Done** | `FieldInfo` carries `tooltip`, `category`, `isAssetPath`, `replicated`, `serialized`, `enumNames`, `visibleWhenField` (confirmed in `Reflection.h`); `RenderReflectedFields` migrated many renderers |
| 2 — Scene serialization | **Done** | `Core/ReflectionSerializer.h` exists; data-driven type map replaces switch cases |
| 3 — Save/Load | **Done (practical)** | Hand-written serializers removed for the common components; a few with custom key formats intentionally kept |
| 4 — Network replication | **Prep done** | `FieldInfo::replicated` exists (`Reflection.h:95`) and is consumed (`Reflection.h:171`); existing `ReplicatedField<T>` system kept |
| 5 — Materials | **Partial** | `PBRProperties` + render state registered; `[Advanced]`/`[Textures]`/`[Variants]` sections stay manual |
| 6 — AngelScript | **Done** | `AutoRegisterReflectedTypes()` + generic `getComponentField`/`setComponentField`/`hasComponent` in `AngelScriptEngine.cpp` |
| 7 — Settings | **Done** | Settings groups migrated to reflection-driven read/write |
| 8B — UI bindings | **Done** | Templated `UITypedBinding<T>` replaces the four concrete classes |
| 8C — Physics backend | **Deferred** | `IPhysicsBackend.h` exists but `PhysicsSystem` does **not** inherit it (confirmed in the header doc); raw-ID vs. named-body API mismatch |
| 8D — Editor panels | **Done** | `PanelCategory` enum + `GetCategory()`; View menu groups by category |
| Components | **Done** | ~73 `SPARK_REFLECT_TYPE` registrations in `ComponentReflection.cpp` (up from 32) |
| Conditional visibility | **Done** | `FieldInfo::visibleWhenField`/`visibleWhenValue` + macro |
| Live editing / undo | **Done** | Reflected edits push `LambdaCommand` through `CommandHistory` |
| Scene `_properties` block | **Done** | `ComponentToJSON`/`JSONToComponent` emit/consume named-field JSON |

Only **Phase 5 (Materials)** is genuinely partial and **Phase 8C (Physics backend)** is genuinely deferred; everything else is functionally complete.

## What Already Exists (foundation)

| Component | File | Status |
|-----------|------|--------|
| `TypeRegistry` singleton | `Core/Reflection.h` | Production |
| `TypeInfo` (name, size, alignment, base, fields, version) | `Core/Reflection.h` | Complete |
| `FieldInfo` (name, type, offset, size, range, attributes) | `Core/Reflection.h` | Complete with attributes |
| `ComponentFactory` (add/has/remove by string) | `Core/Reflection.h` | Production |
| `SPARK_REFLECT_TYPE/FIELD/END` macros | `Core/Reflection.h` | Production |
| `SetFieldFromString` / `GetFieldAsString` | `Core/ComponentReflection.cpp` | Production |
| `ReflectionSerializer` | `Core/ReflectionSerializer.h` | Production (the Phase 2 artifact) |
| `CheckedCast<To>(From)` | `Core/SafeCast.h` | Production |
| `EventBus` type dispatch | `Utils/EventBus.h` | Uses `std::type_index` |

## Phase Detail

### Phase 1 — Inspector Unification (Done)
`FieldInfo` was extended with the editor/serialization attributes; `RenderReflectedFields()` handles category grouping, tooltips, enum dropdowns, vec2/3/4, asset pickers (`isAssetPath`), and conditional visibility. Reflected edits flow through `CommandHistory` for automatic undo/redo.

### Phase 2 — Scene Serialization (Done)
`ReflectionSerializer.h` iterates `TypeInfo.fields` to serialize/deserialize any reflected type. The per-component `switch(ComponentType)` cases were replaced by data-driven `TypeRegistry` iteration shared by both JSON and binary paths.

### Phase 3 — Save/Load (Done, practical)
Reflected serializers cover the common components; four hand-written ones (MeshRenderer, Camera, ActiveComponent, AudioSourceComponent) were removed. Components with custom short-key/comma-vector formats were intentionally kept.

### Phase 4 — Network Replication (Prep done)
`FieldInfo::replicated` exists and is consumed during field iteration. The plan to fully replace `EntityReplicator`'s manual indices was de-scoped because the existing `ReplicatedField<T>` system is already well-designed.

### Phase 5 — Materials (Partial)
`PBRProperties` and the render-state struct are reflection-driven. `[Advanced]`, `[Textures]`, and `[Variants]` sections (maps, not flat structs) remain manual.

### Phase 6 — AngelScript (Done)
`AutoRegisterReflectedTypes()` runs at script-engine init; generic `getComponentField`/`setComponentField`/`hasComponent` let scripts reach any reflected component by name. Manual bindings kept for complex APIs (math, physics queries, input).

### Phase 7 — Settings (Done)
Settings groups migrated to a reflection-driven read/write loop, collapsing hundreds of manual read/write lines into one-liners plus registrations.

### Phase 8 — Polymorphism Cleanup
- **8B (UI bindings):** Done — `UITypedBinding<T>` with backward-compatible aliases.
- **8C (Physics backend):** Deferred — `IPhysicsBackend` (raw `uint32_t` IDs) mismatches `PhysicsSystem`'s higher-level named-body / `PhysicsBody*` API; would need an adapter layer, not simple inheritance. `PhysicsSystem` still does not inherit `IPhysicsBackend`.
- **8D (Editor panels):** Done — `PanelCategory` enum, `GetCategory()`, dynamic View-menu grouping.

## Source & Freshness

- **Original entry date:** 2026-04-12 (`reflection-polymorphism-refactoring-plan-2026-04-12.md`, type: Plan)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Foundation confirmed** — `Core/Reflection.h` and `Core/ReflectionSerializer.h` both present; `FieldInfo::replicated` at `Reflection.h:95`, consumed at `:171`.
  - **~73 `SPARK_REFLECT_TYPE` registrations** in `ComponentReflection.cpp` (plan started near 32–40).
  - **AngelScript auto-registration confirmed** — `AutoRegisterReflectedTypes` / `getComponentField` / `setComponentField` present in `AngelScriptEngine.cpp`.
  - **Phase 8C still deferred** — `IPhysicsBackend.h` self-documents that `PhysicsSystem` uses it directly but does not inherit it.
  - **Phase 5 still partial** — material `[Advanced]`/`[Textures]`/`[Variants]` sections remain manual.

## Related Pages

- [Jolt Physics Integration](Jolt-Physics-Integration.md) — Phase 8C context (`IPhysicsBackend`)
- [GPU/CPU Separation Plan](GPU-CPU-Separation-Plan.md)
