# SparkEditor ⇄ Engine ECS Round-Trip — Design (Sub-project 1)

Date: 2026-07-05
Status: Approved (design). Foundation for the Unity/Unreal-style editor effort.

## Context & problem

SparkEditor and the SparkEngine runtime currently use **two disconnected scene/component
models**:

- The editor operates on its own `SparkEditor::SceneFile` (~90 private component POD structs in
  `SparkEditor/Source/SceneSystem/SceneFileTypes.h`). Its Hierarchy and Inspector edit that
  in-memory model. Its Scene View (`Panels/SceneViewPanel.cpp`) renders **no geometry** (a
  blue/green split placeholder), gizmos (`Gizmos/GizmoSystem.cpp` — real math) aren't overlaid on
  it, and **Ctrl+S is lossy**: `EditorUI::SaveCurrentScene` writes only object names + an identity
  transform, discarding all component/transform/mesh/material edits (real serializers exist under
  `SparkEditor/Source/SceneSystem/` but aren't on the save path).
- The runtime game (TERRAFRONT / `GameModules/SparkGameMMOFPS`) bypasses the editor entirely: it
  loads engine `.scene` (INI) files + JSON data tables directly in C++ and renders through its own
  path (`TFWorldSetup::RenderWorld`).

Consequently, "drop an object into the world, edit it, save, and see it in-game" is impossible at
any layer — the author→save→load→render loop is broken on both sides and the two sides speak
different languages.

The engine already provides reusable infrastructure that is 80% there but unconnected:
- **Reflection**: `SparkEngine/Source/Core/Reflection.h` + `ComponentReflection.cpp` register ~90
  **real engine ECS component types** into a global `TypeRegistry`, with per-field `FieldInfo`
  (name, type, offset, size, range, category, tooltip, **`isAssetPath`**, enumNames,
  visibleWhen…), plus `SetFieldFromString`/`GetFieldAsString` and a `ComponentFactory`
  (add/has/remove/get by type name).
- **Generic inspector renderer**: `InspectorPanel::RenderReflectedFields(void*, vector<FieldInfo>)`
  (`InspectorComponentRenderers_Reflected.cpp`) already renders bool/int/float/double/string/vec2-4/
  enum with categories/ranges/tooltips/conditional visibility and routes edits through
  `CommandHistory` (undo/redo). Today it is fed the editor's `SceneFile` structs, not `TypeRegistry`.
- Real `SceneFile` JSON/binary serializers, a working undoable Hierarchy
  (`Panels/HierarchyPanel.cpp`), real gizmo math, a real Material editor, `ProjectManager`, an
  `AssetDatabase` (GUIDs) — most reusable, some to be repointed.

## Decision (approved)

**Engine ECS is the canonical model.** The editor operates on the engine's real ECS `World` and a
generic reflection-based scene format. The editor's own `SceneFile` component structs are no longer
the thing being edited; the editor edits real engine components (which are already reflected). The
runtime loads the same format. No permanent translation layer.

## Goals (this slice)

1. The Scene View renders the **real engine ECS World** (entities with `Transform` +
   `MeshRenderer` + material) with an editor fly-camera and gizmo overlay on the selection.
2. The Inspector edits the **selected entity's real engine components** generically via the existing
   reflected-field renderer fed by `TypeRegistry`, with undo/redo.
3. The Hierarchy shows/creates/deletes/renames/reparents **real ECS entities**.
4. **Ctrl+S saves full component data** to a generic reflection-based JSON scene; Load reads it back
   with round-trip fidelity.
5. The **runtime loads that same file** and renders it identically.

Acceptance gate: *place & transform an entity that has a mesh in the editor, Save, launch the
runtime on that file, and it renders identically.*

## Non-goals (later sub-projects — explicitly out)

- Asset browser thumbnails & drag-drop; inspector asset-path *pickers* (browse button / drag-drop
  target honoring `isAssetPath`). → Sub-project 2.
- Drag-a-model-into-viewport to spawn; script attachment (`Script.scriptPath` → `AttachScript` at
  load). → Sub-project 3.
- Migrating TERRAFRONT off its INI `.scene` + hardcoded C++ paths onto this format. → Sub-project 4.
- Converting the ~90 editor `SceneFile` structs or the legacy INI loader. The INI loader keeps
  working; TERRAFRONT is untouched by this slice.

## Architecture — four units

### Unit A — Canonical scene format + reflection serializer (engine-side, shared)

A game-agnostic JSON scene format:

```json
{
  "version": 1,
  "entities": [
    { "name": "Ground", "id": 1, "parent": 0,
      "components": [
        { "type": "Transform", "fields": { "position": "0 0 0", "rotation": "0 0 0", "scale": "1 1 1" } },
        { "type": "MeshRenderer", "fields": { "meshPath": "Assets/Models/…/x.obj", "materialPath": "Assets/Materials/…/y.json" } }
      ] }
  ]
}
```

- **Serialize**: for each entity, for each component present, walk its `TypeRegistry` `FieldInfo`
  list and emit `field.name → GetFieldAsString(component_ptr, field)`. Zero per-type code — any
  reflected component serializes automatically.
- **Deserialize**: for each component entry, `ComponentFactory::Add(world, entity, type)` then
  `SetFieldFromString(component_ptr, field, value)` per field. Unknown types/fields are skipped
  with a logged warning (forward-compat).
- Location: new `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.{h,cpp}` in
  `SparkEngineLib` so both the editor exe and the runtime link it.
- Interface (sketch):
  `bool SaveWorld(const World&, const std::string& path);`
  `bool LoadWorld(World&, const std::string& path);`
  plus `std::string SerializeWorld(const World&)` / `bool DeserializeInto(World&, std::string_view)`
  for testing without disk.
- Entity id + parent are serialized as stable integer handles for reparenting; a load-time id→entity
  map resolves parents in a second pass.

### Unit B — Editor scene renderer

Renders the engine `World` into the Scene View's existing D3D11 render-target.

- The editor exe already owns a `GraphicsEngine`/device (SceneViewPanel creates a render-texture).
  Add an `EditorSceneRenderer` that, given a `World`, a view/proj from an editor camera, and the RT:
  iterate `GetEntitiesWith<Transform, MeshRenderer>`, load each mesh via a device-direct
  tinyobj/`LoadOrPlaceholderMesh` cache (the proven path), bind material texture, draw with the
  basic shader. Same technique validated in `TFWorldSetup::RenderWorld` (device-direct instead of
  the AssetPipeline OBJ path, which is unreliable on Windows).
- Editor fly-camera: WASD + RMB-look + scroll (replace SceneViewPanel's orbit-distance proxy).
- Gizmo overlay: feed the existing `GizmoSystem` the selected entity's `Transform*`; it already
  ray-picks and applies deltas with undo snapshots. Wire it to draw over the rendered RT and consume
  viewport mouse input.
- Location: `SparkEditor/Source/Rendering/EditorSceneRenderer.{h,cpp}`; `SceneViewPanel` calls it
  instead of `RenderSceneContent`'s placeholder.

### Unit C — Inspector + Hierarchy repointed to the real ECS World

- **Editor owns a live engine `World`** (`m_world`) as the document being edited (held by the
  editor document/scene state, created on new/open scene).
- **Hierarchy**: create/delete/duplicate/rename/reparent operate on `m_world` entities (entt
  create/destroy + a name component + parent component). Selection is an `EntityID`. Keep the
  existing undoable command wrappers; their payload becomes entity ops.
- **Inspector**: for the selected entity, enumerate its present components (via `ComponentFactory` /
  a registry query), and for each call `RenderReflectedFields(component_ptr, TypeRegistry fields)`.
  The generic renderer already exists; the change is *feeding it real component memory + TypeRegistry
  FieldInfo* instead of `SceneFile` blobs. Add-Component menu lists reflected component type names.
  Asset-path fields stay plain text this slice (picker is sub-project 2), but `isAssetPath` is
  surfaced read-only so sub-project 2 can attach the picker at one choke point.
- Undo/redo: `CommandHistory` diffs the edited component's bytes before/after (offset+size known from
  `FieldInfo`), same pattern as today.

### Unit D — Runtime loader

- The runtime (initially a standalone verification path; TERRAFRONT migration is later) gains
  `LoadWorld(World&, path)` from Unit A. For the acceptance test we drive it via a small runtime
  entry (or a `-scene <file>` path on an existing headless/windowed runtime) that loads the file into
  a `World` and renders it with the same generic ECS render used by the editor (extract the
  device-direct render into a shared helper if cheap; otherwise duplicate minimally and unify later).

## Data flow (the round-trip)

`Editor World (entt) → reflect-serialize → scene.json → reflect-deserialize → Runtime World (entt) → render`.
The identical serialize/deserialize code (Unit A) runs on both ends, so round-trip fidelity is
structural, not hand-maintained.

## Testing

- **Unit A round-trip test** (no GPU, no editor): build a `World` in code with a handful of entities
  carrying Transform + MeshRenderer (+ a couple of scalar components), `SerializeWorld`,
  `DeserializeInto` a fresh `World`, assert every entity/component/field matches
  (`GetFieldAsString` equality across all reflected fields) and parent links resolve. Add to the
  existing `SparkTests` suite (CI-safe, no device).
- **Reflection coverage test**: assert every field type the serializer emits is round-trippable by
  `SetFieldFromString(GetFieldAsString(x)) == x` for representative values (guards silent data loss
  on unsupported field types).
- **Editor smoke (manual/scripted)**: open editor, create entity + MeshRenderer, transform it, Save;
  reload the file → identical hierarchy/fields. Then runtime loads the same file and renders it
  (screenshot compare against the acceptance gate).

## Risks & mitigations

- **Two reflection worlds** (`TypeRegistry` over engine ECS vs. Inspector's inline `SceneFile`
  descriptors). Mitigation: this slice *repoints* the Inspector to `TypeRegistry`; the `SceneFile`
  inline descriptors are left in place but unused for the ECS path (removed opportunistically, not
  as a blocking refactor).
- **Editor rendering the engine World.** The editor exe links `SparkEngineLib` statically (no DLL
  boundary like the game module had), so `EngineContext`/global-registry pitfalls that bit the game
  module do not apply. Still, use the device-direct mesh path (not the AssetPipeline OBJ loader) —
  it's the proven-reliable one on Windows.
- **Field types the serializer can't stringify** (e.g. asset handles, nested structs, arrays).
  Mitigation: the coverage test flags them; for this slice, scope to the scalar/vec/string/enum
  field types the generic renderer already supports, and log+skip anything else (documented gap for
  a later pass).
- **Scope creep into asset pickers / drag-drop / scripts.** Held out by the non-goals; the design
  leaves single choke points (`isAssetPath` on fields, the Inspector field renderer) for those to
  bolt on later without rework.

## Sequencing after this slice

2. Asset workflow (thumbnails, drag-drop, inspector asset pickers honoring `isAssetPath`).
3. Placement (drag model → spawn) + script attachment (`Script.scriptPath` → `AttachScript` at load).
4. TERRAFRONT de-hardcode: migrate the hardcoded C++ paths/values (soldier/prop/skybox/terrain-tex
   meshes, the faction-material switch duplicated in 4 files, viewmodel/FX magic numbers, wind audio)
   onto the canonical data model + load its scene through the new loader.
