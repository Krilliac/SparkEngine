---
name: sparkengine-editor-scenes-and-reflection
description: SparkEditor scene lifecycle, the two JSON scene dialects (reflected-World v1 vs SceneFile v2 fail-closed schema), reflection-driven Inspector property editing, reflection registration (SPARK_REFLECT macros, TypeRegistry/ComponentFactory mechanics), and CommandHistory undo/redo. TRIGGER when the user says "scene won't load", "scene file version is unsupported", "sparkscene format", "save scene / open scene in the editor", "scene round trip loses data", "Inspector doesn't show my field", "register a component for reflection", "SPARK_REFLECT", "no reflection data registered", "property edit isn't undoable", "Ctrl+Z broken", "undo crashes after opening a scene", "binary scene", "schema version", or "scene backup/.bak". DO NOT TRIGGER for save-game (.spark_save) or database persistence-format migrations and serialization primitives (use sparkengine-persistence-save-and-migrations), for ECS phase/world lifecycle and threading (use sparkengine-ecs-lifecycle-threading-and-memory), or for packaging/launching builds (use sparkengine-run-package-and-release).
---

# SparkEditor scenes, reflection-driven inspection, and undo/redo

Runbook for a zero-context engineer working on: opening/saving scenes in the ImGui
editor, the on-disk scene formats and their version gates, the reflection-driven
Inspector, and the CommandHistory undo/redo system. All paths are repo-relative.

**Jargon used once, defined once:**

- **World** — the live ECS document (EnTT registry wrapper, `::World`). The running
  editor edits a `World`, owned by `EditorUI::m_world`.
- **SceneFile** — an in-memory editor data model (`SparkEditor::SceneFile`,
  `SparkEditor/Source/SceneSystem/SceneFile.h`): header + objects + components +
  environment. A *separate* document type from `World`.
- **Dialect** — one of the two incompatible JSON scene layouts described below.
- **Fail-closed** — on any version/schema mismatch the loader returns an error and
  touches nothing, instead of guessing or partially loading.
- **CommandHistory** — the process-wide undo/redo singleton
  (`SparkEditor/Source/CommandHistory.h`, wrapping
  `SparkEditor/Source/UndoRedo/UndoRedoManager.h`).

## The one fact that prevents most confusion: there are TWO scene dialects

Both can live in a `.sparkscene` file. They are not interchangeable, and each
loader rejects the other's files cleanly.

| | Dialect A: reflected-World JSON | Dialect B: SceneFile JSON v2 |
|---|---|---|
| Code | `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.{h,cpp}` (`Spark::SaveWorld/LoadWorld/SerializeWorld/DeserializeInto`) | `SparkEditor/Source/SceneSystem/SceneSerializer.{h,cpp}` + `JSONSceneSerializer.cpp` + `SceneComponentCodec.{h,cpp}` |
| Document type | live ECS `World` | `SparkEditor::SceneFile` |
| Top-level shape | `{"version": 1, "entities": [...]}` | `{"version": 2, "objects": [...], "components": [...], "environment": {...}, ...}` |
| Version gate | `"version": 1` is **written but not checked** on load; load only requires an `entities` array | `version` must appear **exactly once** and equal `SCENE_FILE_VERSION` (= 2) or the load fails closed |
| Per-component schema | Fields serialized **as strings** via reflection (`TypeRegistry`/`ComponentFactory`); only fields with `serialized == true`; unknown component types are **skipped with a warning** | Non-marker components require `data: {"schema": 1, "fields": {...}}` where 1 = `SCENE_COMPONENT_SCHEMA_VERSION`; unknown types, wrong schema, or malformed payloads **reject the whole file** |
| Who uses it in the running editor | **File → Open / Save** (`EditorUI::OpenScene` / `EditorUI::SaveCurrentScene` in `SparkEditor/Source/Core/EditorUI.cpp`, "full-fidelity scene round-trip (C4)") | SceneFile-backed panels (legacy Hierarchy/Inspector fallback path, Physics2D/3D, Tilemap, PostProcessing panels, PrefabManager) and `SceneManager` |
| Tests | `Tests/TestReflectedScene.cpp`, `Tests/TestReflectedSceneEmissiveHierarchy.cpp` | `Tests/TestSceneSerializer.cpp`, `Tests/TestSceneSerializerReal.cpp` |

**Identify a file's dialect in 5 seconds:** top-level `"entities"` array → Dialect A.
Top-level `"objects"` + `"version": 2` → Dialect B.

Cross-loading fails cleanly in both directions (verified in code): Dialect B's
loader rejects A's files because `version` ≠ 2; Dialect A's loader rejects B's
files because there is no `entities` array (`LoadWorld` returns `false`).

### Extensions and format detection (Dialect B)

`SceneSerializer::DetectFormat` (in `SceneSystem/SceneSerializer.cpp`) recognizes
only `.sparkscene`, `.json`, `.scenejson` — all mapped to JSON. Anything else →
"Unsupported scene file extension" error. `GetSupportedExtensions(BINARY)` returns
an **empty list**.

### Binary scenes: fail-closed, do not "fix" this by re-enabling

`SceneSystem/BinarySceneSerializer.cpp` — `SaveBinary`/`LoadBinary` unconditionally
return an error ("Binary scene serialization is unavailable because the legacy
format is incomplete; use JSON/.sparkscene"), and `DeserializeComponent` returns
`false` ("legacy raw payloads fail closed"). Serializing raw C++ object images was
deliberately retired (`SceneFileTypes.h` `Component::data` comment: "Persisted
bytes are produced only by the versioned scene-component codec; object images are
never retained"). The `SerializationFormat::BINARY` enum value is documented as
"Reserved; currently rejected". Any request to bring binary scenes back is a
schema-design task, not a bug fix — do not delete the guards.

## Scene-version semantics (all four version numbers)

| Constant | Value | Where | Load behavior on mismatch |
|---|---|---|---|
| `SCENE_FILE_VERSION` | 2 | `SceneSystem/SceneFileTypes.h` | Exact match required, field must appear exactly once → whole-file rejection: "Scene file version is unsupported; legacy raw-memory scene payloads must be resaved by a trusted build" |
| `SCENE_COMPONENT_SCHEMA_VERSION` | 1 | `SceneSystem/SceneComponentCodec.h` | Per-component `data.schema` must equal 1, `data` must contain exactly `schema` + `fields` → whole-file rejection |
| Reflected-World `"version"` | 1 | `SceneManager/ReflectedSceneSerializer.cpp` (`root["version"] = 1`) | Written on save, **not validated on load** (only `entities` is required) — labeled `open`: a future v2 has no gate yet |
| `TypeInfo::version` (`SPARK_REFLECT_VERSION`) | per-type | `SparkEngine/Source/Core/Reflection.h` | Reflection schema version for save-file migration; migration mechanics belong to sparkengine-persistence-save-and-migrations |

There is **no scene migration path**: `SceneSerializer::HandleVersionCompatibility`
rejects any `fileVersion != 2` with a warning; it never upgrades. Old files must be
resaved by a build that wrote them. (Verified: `TestSceneSerializerReal.cpp` feeds
`{"version":3}` and a header with `SCENE_FILE_VERSION + 1` and expects rejection.)

### Dialect B hardening behaviors (all verified in `JSONSceneSerializer.cpp`)

- **All-or-nothing load**: parses into a local `SceneFile loadedScene` and publishes
  only after full validation — "Failed loads must not append to or partially
  overwrite the caller's live scene."
- **Marker-only components**: `TRANSFORM` and `SPRITE_ANIMATOR` must NOT carry a
  `data` object; presence of one is a rejection.
- **Semantic payload validation**: out-of-range payloads (e.g. invalid enum ints
  for `RIGID_BODY_2D`, `LOD_GROUP`, `VEHICLE`, `TEXT_3D`, `HEALTH`,
  `PARTICLE_SYSTEM`) are rejected via the codec, not clamped.
- **Atomic save**: writes to a temporary sibling file, size-checks against
  `m_maxFileSize` (100 MB), then `ReplaceFileAtomically`; partial files are removed.
- **`.bak` backup**: `SaveScene` copies the existing file to `<path>.bak` first
  (`m_createBackups` defaults true; backup failure is non-fatal).
- String capacity: scene name ≥ 64 or description ≥ 256 bytes, or embedded NULs →
  rejection (matches the fixed `char[]` header fields).

## Editor scene lifecycle (the SwapWorld funnel)

All document replacement goes through `EditorUI::SwapWorld(std::unique_ptr<::World>)`
(`SparkEditor/Source/Core/EditorUI.cpp`), which does, in this exact order:

1. `CommandHistory::GetInstance().Clear()` — **before** the old World is freed.
   Commands (LambdaCommands) close over raw entity/component pointers of the old
   World; clearing first makes undo-into-freed-World (use-after-free) impossible.
2. `m_world = std::move(newWorld)` — old World freed here.
3. `RewirePanelsToWorld()` — SceneView and Hierarchy cache a raw `::World*` and are
   re-pointed; `m_selectedEntity = entt::null` so the Inspector never reflects a
   stale handle.

Callers of the funnel: `OpenScene()`, initial world creation at startup, and editor
teardown (`SwapWorld(nullptr)`). **Decision rule:** never assign `m_world` directly;
never free a World that CommandHistory might still reference. The same pattern
exists in `HierarchyPanel::ResetToDefault()` (clears history before freeing its
owned SceneFile).

Open/save specifics:

- `OpenScene(path)`: loads into a **fresh** World first (`Spark::LoadWorld(*fresh, path)`);
  a failed load never corrupts the current document. Then `SwapWorld`, set
  `m_currentScenePath/Name`, `m_sceneModified = false`, `CommandHistory::MarkSaved()`,
  notify plugins (`NotifySceneLoad`).
- `SaveCurrentScene(path)`: creates parent directories, `Spark::SaveWorld`, then
  `MarkSaved()` + plugin `NotifySceneSave`. The File menu default path is
  `<project scenes dir>/<sceneName>.sparkscene` (`Core/EditorMenuBar.cpp`).
- **Dirty tracking**: `EditorUI::IsSceneModified()` ORs a legacy `m_sceneModified`
  flag with `CommandHistory::IsModified()`, which compares a monotonic mutation
  sequence against the `MarkSaved()` snapshot (`UndoRedoManager` `m_savedSequence`)
  — undoing back to the saved point reads as clean.
- **Exit**: `EditorApplication::OnShutdownRequested()` only **logs a warning** on
  unsaved scene changes and allows exit (it auto-saves the open *project*, not the
  scene). There is no blocking save-prompt modal — `open` gap, do not claim one exists.
- **Recovery dialog** (`EditorUI::ShowRecoveryDialog`): restores the `_recovery`
  *layout* via `EditorLayoutManager` — panel layout only, **not** scene content.

## Reflection-driven inspection and property editing

Two Inspector paths coexist in `SparkEditor/Source/Panels/InspectorPanel.cpp`;
`Render()` prefers the World path whenever `EditorUI` has a World and a selected
ECS entity.

### Path 1 — World-backed (live ECS, preferred)

`RenderWorldBackedInspector(::World*, ::EntityID)`:
- Iterates `Spark::ComponentFactory::Get().GetRegisteredNames()`, renders each
  present component's fields via `Spark::TypeRegistry::Get().FindTypeByName(type)`
  → `RenderReflectedFields(comp, ti->fields)`.
- "Add Component" menu adds via `ComponentFactory::AddComponent`.
- Components without reflection data show "(no reflection data registered)" —
  fix by registering the type in `SparkEngine/Source/Core/ComponentReflection.cpp`
  (the `SPARK_REFLECT_*` macros and `TypeInfo` model live in
  `SparkEngine/Source/Core/Reflection.h`; registration mechanics are owned by
  this skill).
- **Known gap (`open`, verified 2026-08-23):** this path calls
  `RenderReflectedFields` with **no snapshot and no CommandHistory command** —
  World-backed Inspector field edits and Add Component are **not undoable** and do
  not advance the dirty sequence. Do not describe World-inspector edits as undoable.

### Path 2 — SceneFile-backed (legacy fallback, fully undoable)

`InspectorComponentRenderers_ReflectedInternal.h` defines
`RENDER_REFLECTED_COMPONENT(compType, icon, displayName, dataType, fields...)`:
renders fields, `memcmp`s a pre/post snapshot of the POD payload, and on change
executes a `LambdaCommand` through `CommandHistory` capturing old/new snapshots.
Field descriptors are built with `FIELD_FLOAT/INT/BOOL/VEC2/VEC3/VEC4/STRING/ENUM`
+ `FIELD_FLOAT_RANGE` macros (offsetof-based `Spark::FieldInfo`). `RemoveComponent`
is also command-wrapped (captures the removed `Component` for undo).

`RenderReflectedFields` (in `InspectorComponentRenderers_Reflected.cpp`, shared by
both paths) honors `FieldInfo` attributes: `category` sections, `tooltip`,
`readOnly`, `hasRange`, `enumNames` dropdowns, and `visibleWhenField/Value`
conditional visibility.

## Undo/redo contract

- One process-wide stack: `Spark::Editor::CommandHistory::GetInstance()` adapts
  `ICommand` onto `SparkEditor::UndoRedoManager::GetInstance()`. Ctrl+Z / Ctrl+Y /
  Ctrl+Shift+Z are handled in `EditorUI.cpp`.
- `Execute()` clears the redo stack; same-`GetTypeId()` commands may coalesce via
  `MergeWith` (used for continuous drags; `PropertyCommand<T>` merges when typeId ≠ 0).
- `CompoundCommand` groups multi-step edits into one undo step; `LambdaCommand` is
  the quick one-off form (capture by value; captured raw pointers are only safe
  because SwapWorld/ResetToDefault clear history before freeing the document).
- **Bypass guard**: `CommandHistory::WarnIfBypassingDispatch(op)` →
  `UndoRedoManager::WarnIfMutationBypassesDispatch` logs a warning when a mutation
  runs outside command dispatch. It is **log-only** — it does not block or record.
  Treat any such warning in the log as a real defect to wrap in a command.
- **Gizmos**: the SceneView translate-gizmo drag (`Panels/SceneViewPanel.cpp`)
  executes a `LambdaCommand` at drag end — World-entity gizmo moves ARE undoable.
  The older `Gizmos/GizmoSystem.cpp` `ApplyTranslation/Rotation/Scale` mutate
  `Transform*` directly behind the log-only guard and have no non-test editor
  callers found (dormant legacy path — `candidate` for wiring or deletion).

## Failure modes → first moves

| Symptom | Cause / where to look |
|---|---|
| "Scene file version is unsupported; legacy raw-memory scene payloads must be resaved…" | Dialect B loader saw `version` ≠ 2, duplicated, or missing. No migration exists — resave from a trusted build. |
| "Scene component requires a registered schema-tagged data object" / "data schema is invalid" | Component `data` missing `{"schema":1,"fields":{}}` shape, or type has no codec (`HasSceneComponentPayloadCodec` in `SceneComponentCodec.cpp`). |
| "Marker-only scene component must not contain data" | `TRANSFORM`/`SPRITE_ANIMATOR` entry carries a `data` object — remove it. |
| File → Open fails, console: "Failed to open scene (Spark::LoadWorld)" | Wrong dialect (no `entities` array), unparseable JSON, or unreadable path. Current document is untouched by design. |
| Loaded scene silently missing a component | Dialect A skips unknown component types with `[ReflectedScene] unknown component type '<x>' skipped` — the type isn't registered in `ComponentFactory` (`SparkEngine/Source/Core/ComponentReflection.cpp`). |
| Field saves but doesn't round-trip | Dialect A only round-trips `serialized == true` fields of scalar/string/vector/enum `FieldType`s; unsupported types log `[ReflectedScene] skip unsupported field`. |
| Editor crash (often UAF) after File → Open, or Ctrl+Z crashes post-open | A code path replaced the World without `SwapWorld` — see sparkengine-debugging-playbook §"Editor crashes / UAF when opening a scene". |
| Property edit not undoable | You are on the World-backed Inspector path (known gap above), or the mutation bypassed dispatch (check log for "bypassed UndoRedoManager dispatch"). |
| Editor exits without prompting to save | Expected today: exit only logs a warning (`EditorApplication::OnShutdownRequested`). |
| "Binary scene serialization is unavailable…" | Working as designed; use `.sparkscene` JSON. |

## Status ledger (do not oversell)

| Claim | Status |
|---|---|
| SceneFile v2 fail-closed load/save, schema-tagged codec, atomic write | Implemented + tested (`TestSceneSerializer.cpp`, `TestSceneSerializerReal.cpp`, registered in `Tests/CMakeLists.txt`) + CI-enforced (blocking `build-linux-gcc/clang` and `build-windows-vs2022` jobs run `ctest` with `-DBUILD_TESTS=ON`) |
| Reflected-World round trip (transform, mesh, enums/masks, hierarchy) | Implemented + tested (`TestReflectedScene.cpp`, `TestReflectedSceneEmissiveHierarchy.cpp`, registered) + CI-enforced |
| Undo/redo stack, merge, saved-sequence dirty tracking | Implemented + tested (`TestUndoRedoManager.cpp`, `TestCommandHistory.cpp`, registered) + CI-enforced |
| SwapWorld history-clear ordering (UAF prevention) | Implemented; enforced by code structure + debugging-playbook check; no dedicated automated test found — treat as code-reviewed, not test-proven |
| Reflected-World load-time version gate | `open` — version written, never checked |
| World-backed Inspector undo support | `open` — edits bypass CommandHistory |
| Blocking unsaved-changes exit prompt | `open` — log-only today |
| `SparkEditor::SceneManager` (`SceneSystem/SceneManager.{h,cpp}`) | `candidate` — compiled but **no instantiation found anywhere** in editor/engine/tests (the `SceneManager` built in game modules is the engine class, a different type). Per the project's wire-in-or-delete rule, wire it or delete it before building on it. |

## Sibling routing

| Need | Skill |
|---|---|
| Register/reflect a new component type, `SPARK_REFLECT_*` macros, TypeRegistry/ComponentFactory mechanics | **this skill** (anchors: `Core/Reflection.h`, `Core/ComponentReflection.cpp`) |
| Save-game `.spark_save` format, AsyncDatabase, serialization primitives, persistence-format **migrations** | sparkengine-persistence-save-and-migrations |
| ECS phase ordering, World/threading/memory lifecycle | sparkengine-ecs-lifecycle-threading-and-memory |
| Packaging, launching, release gates | sparkengine-run-package-and-release |
| Editor crash triage (UAF on open, panel wiring) | sparkengine-debugging-playbook |

## Verify before you trust (copy-paste)

```bash
# Build + run all tests (single CTest entry "SparkEngineTests" wraps the SparkTests binary)
cmake --preset windows-release                 # or linux-gcc-release
cmake --build --preset windows-release
ctest --test-dir build/windows-release -C Release --output-on-failure

# Run only the scene/undo tests (env-var filter — full selector list:
# sparkengine-validation-and-qa §3; preset builds put the binary in build/<preset>/bin/)
SPARK_TEST_FILE=TestSceneSerializerReal.cpp ./build/windows-release/bin/SparkTests
SPARK_TEST_FILE=TestReflectedScene.cpp     ./build/windows-release/bin/SparkTests
SPARK_TEST_FILE=TestUndoRedoManager.cpp    ./build/windows-release/bin/SparkTests
```

A run that lists 0 tests proves nothing — confirm the filter actually matched
(the runner prints the executed count).

## Provenance and maintenance

Authored 2026-08-23 against branch `claude/whole-nine-yards-20260823`; every claim
verified by reading the cited files in this repo on that date. Re-verify lines:

```bash
# Scene versions still 2 / 1
grep -n "SCENE_FILE_VERSION = " SparkEditor/Source/SceneSystem/SceneFileTypes.h
grep -n "SCENE_COMPONENT_SCHEMA_VERSION = " SparkEditor/Source/SceneSystem/SceneComponentCodec.h
# Fail-closed gates still in place
grep -n "version is unsupported" SparkEditor/Source/SceneSystem/JSONSceneSerializer.cpp
grep -n "Refusing unsupported binary scene" SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp
# Reflected dialect still writes version 1 and requires "entities"
grep -n "root\[\"version\"\] = 1\|contains(\"entities\")" SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp
# Editor menu still routes Open/Save through the reflected serializer + SwapWorld
grep -n "SaveWorld\|LoadWorld\|SwapWorld" SparkEditor/Source/Core/EditorUI.cpp | head
# World-backed Inspector still lacks command wrapping (gap open while true)
grep -n "RenderReflectedFields" SparkEditor/Source/Panels/InspectorPanel.cpp
# Editor SceneManager still unwired (no output = still unwired)
grep -rn "SceneSystem/SceneManager.h" SparkEditor/Source --include="*.cpp" | grep -v SceneSystem/SceneManager.cpp
# Tests still registered
grep -n "TestSceneSerializerReal\|TestReflectedScene\|TestUndoRedoManager\|TestCommandHistory" Tests/CMakeLists.txt
```
