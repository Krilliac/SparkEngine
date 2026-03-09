# SparkEditor — Gap Analysis

> **Scope**: SparkEditor subsystem (`SparkEditor/Source/`)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of every `.h`/`.cpp` under `SparkEditor/Source/`.
> Each gap is assigned a severity: **Critical** (blocks editor usability), **Major** (significant missing functionality), **Moderate** (partial implementation, workaround exists), **Minor** (polish / quality-of-life).

---

## Critical Gaps

### GAP-E01 — GizmoSystem is 100 % Stubbed (22/22 Methods)

**Files**:
- `SparkEditor/Source/Gizmos/GizmoSystem.h`
- `SparkEditor/Source/Gizmos/GizmoSystem.cpp`

**Impact**: Without a working gizmo system there is no way to translate, rotate, or scale objects in the Scene View. This is the single most fundamental editor interaction.

**Evidence** — every public method returns a default value or is empty:

```cpp
// GizmoSystem.cpp
bool GizmoSystem::Initialize()                    { return false; }      // line 24
void GizmoSystem::Shutdown()                      {}                     // line 29
void GizmoSystem::Update()                        {}                     // line 31
void GizmoSystem::Render()                        {}                     // line 33
bool GizmoSystem::HandleMouseInput(...)            { return false; }      // line 38
void GizmoSystem::RenderTranslationGizmo(...)      {}                     // line 47
void GizmoSystem::RenderRotationGizmo(...)         {}                     // line 52
void GizmoSystem::RenderScaleGizmo(...)            {}                     // line 57
GizmoAxis GizmoSystem::TestTranslationGizmoHit(...){ return NONE; }      // line 62
GizmoAxis GizmoSystem::TestRotationGizmoHit(...)   { return NONE; }      // line 67
GizmoAxis GizmoSystem::TestScaleGizmoHit(...)      { return NONE; }      // line 72
void GizmoSystem::ApplyTranslation(...)            {}                     // line 77
void GizmoSystem::ApplyRotation(...)               {}                     // line 79
void GizmoSystem::ApplyScale(...)                  {}                     // line 83
float GizmoSystem::SnapToGrid(...)                 { return 0.0f; }      // line 87
float GizmoSystem::SnapToRotation(...)             { return 0.0f; }      // line 92
Vec3  GizmoSystem::CalculateGizmoCenter(...)       { return {0,0,0}; }   // line 97
float GizmoSystem::CalculateAdaptiveSize(...)      { return 1.0f; }      // line 102
Color GizmoSystem::GetAxisColor(...)               { return white; }     // line 107
bool  GizmoSystem::CreateGizmoGeometry()           { return false; }     // line 112
bool  GizmoSystem::CreateGizmoShaders()            { return false; }     // line 117
```

**Also stubbed**: `Ray::ScreenToWorldRay` (line 9) — returns a default ray with no screen-to-world math.

**What is needed**: Implement translation/rotation/scale gizmo rendering via immediate-mode line drawing or pre-built geometry, hit-testing against the gizmo axes, and delta application to the selected entity's transform. Integrate with `CommandHistory` for undo/redo.

---

### GAP-E02 — AdvancedAssetPipeline is 100 % Stubbed (57 Methods)

**Files**:
- `SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.h`
- `SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.cpp` (293 lines)

**Impact**: No asset can be processed, compressed, validated, or have thumbnails generated. The entire import pipeline beyond raw file copy is non-functional.

**Evidence** — representative stubs:

```cpp
// AdvancedAssetPipeline.cpp
bool TextureProcessor::Process(...)         { return false; }  // line 20
bool TextureProcessor::GenerateThumbnail(...){ return false; }  // line 26
bool TextureProcessor::CompressTexture(...) { return false; }  // line 36
bool MeshProcessor::Process(...)            { return false; }  // line 54
bool MeshProcessor::OptimizeMesh(...)       { return false; }  // line 70
bool MeshProcessor::GenerateLightmapUVs(...){ return false; }  // line 86
bool AudioProcessor::Process(...)           { return false; }  // line 98
bool AudioProcessor::ConvertAudio(...)      { return false; }  // line 114
```

**Dependency graph** — also completely empty:

```cpp
void  AssetDependencyGraph::AddAsset(...)                 {}                 // line 127
void  AssetDependencyGraph::AddDependency(...)            {}                 // line 131
vector AssetDependencyGraph::GetDependencies(...)         { return {}; }     // line 135
vector AssetDependencyGraph::GetProcessingOrder(...)      { return {}; }     // line 145
vector AssetDependencyGraph::DetectCircularDependencies() { return {}; }     // line 151
```

**Pipeline orchestration** — all empty:

```cpp
bool AdvancedAssetPipeline::ProcessAsset(...)         { return false; }  // line 186
int  AdvancedAssetPipeline::ProcessAssetsBatch(...)   { return 0; }     // line 192
int  AdvancedAssetPipeline::ScanDirectory(...)        { return 0; }     // line 215
bool AdvancedAssetPipeline::ExportAssetDatabase(...)  { return false; } // line 236
bool AdvancedAssetPipeline::ImportAssetDatabase(...)  { return false; } // line 241
void AdvancedAssetPipeline::ProcessingThreadFunction(){}                // line 262
```

**What is needed**: At minimum, implement `TextureProcessor` (load PNG/JPG/TGA via stb_image, generate mipmaps, compress to DDS/BC formats), `MeshProcessor` (load FBX/OBJ via assimp, optimize with meshoptimizer), and `AudioProcessor` (WAV/OGG decode + resampling). Wire up dependency graph for incremental rebuilds.

---

### GAP-E03 — SceneViewPanel Renders No Actual 3D Scene

**Files**:
- `SparkEditor/Source/Panels/SceneViewPanel.h`
- `SparkEditor/Source/Panels/SceneViewPanel.cpp` (401 lines)

**Impact**: The Scene View — the primary viewport for level editing — draws either a grid placeholder or a gradient background instead of rendering the actual scene.

**Evidence**:

```cpp
// SceneViewPanel.cpp:62-80  —  Placeholder when m_srv is null
// Placeholder when no render texture is available
ImDrawList* drawList = ImGui::GetWindowDrawList();
ImVec2 pos = ImGui::GetCursorScreenPos();

// Draw a simple grid pattern as placeholder
drawList->AddRectFilled(pos, ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                        IM_COL32(50, 50, 50, 255));
// Draw grid lines ...
```

```cpp
// SceneViewPanel.cpp:219-228  —  "Skybox-style gradient" placeholder
// Render a skybox-style gradient background as scene placeholder
// Clears render target with sky blue (0.4, 0.6, 0.9)
```

**Additional sub-gaps within SceneViewPanel**:
- `RenderMode` enum (Shaded / Wireframe / Unlit / Normals / Depth) is declared but has no effect on rendering — there is no rendering to vary.
- `GizmoMode` enum is declared but no gizmo drawing occurs (ties to GAP-E01).
- Camera is orbit-only; WASD keys are approximated by adjusting orbit distance (line 394: `m_cameraDistance -= (dx + dy + dz) * 0.1f`) instead of implementing a proper FPS fly camera.

**What is needed**: Integrate with `GraphicsEngine` to render the actual scene graph into an off-screen render target, then present via `ImGui::Image`. Implement render mode switching, flythrough camera, and wire up gizmo overlay.

---

### GAP-E04 — EditorLayoutManager is 100 % Stubbed (14 Methods)

**File**: `SparkEditor/Source/Core/EditorLayoutManager.h` (84 lines, header-only)

**Impact**: Panel layouts cannot be saved, loaded, or restored. The editor cannot remember window arrangements between sessions.

**Evidence** — every method is an inline no-op:

```cpp
// EditorLayoutManager.h:62-77
bool Initialize(const std::string& layoutDirectory = "Layouts") { return true; }
void Shutdown() {}
void Update(float deltaTime) {}
void BeginFrame() {}
void EndFrame() {}
bool SaveCurrentLayout(const std::string& name, ...) { return true; }
bool LoadLayout(const std::string& name) { return true; }
bool ApplyLayout(const std::string& name) { return true; }
void ResetToDefault() {}
void RegisterPanel(const PanelConfig& config) {}
void SetPanelVisible(const std::string& panelName, bool visible) {}
bool IsPanelVisible(const std::string& panelName) const { return true; }
bool BeginPanel(const std::string& panelName) { return true; }
void EndPanel() {}
```

No `.cpp` file exists — this is entirely header-defined stubs.

**What is needed**: Serialize/deserialize ImGui docking layout via `ImGui::SaveIniSettingsToMemory()` / `ImGui::LoadIniSettingsFromMemory()`. Persist to JSON files under a `Layouts/` directory. Implement `RegisterPanel` to track panel metadata.

---

### GAP-E05 — MaterialEditor is 100 % Stubbed (33 Methods)

**Files**:
- `SparkEditor/Source/MaterialEditor/MaterialEditor.h` (574 lines)
- `SparkEditor/Source/MaterialEditor/MaterialEditor.cpp` (150 lines)

**Impact**: No material can be created, edited, compiled, or previewed. The node-graph material editor — a key workflow for artists — is entirely non-functional.

**Evidence**:

```cpp
// MaterialEditor.cpp
void MaterialEditor::CreateNewMaterial()               {}                // line 32
bool MaterialEditor::LoadMaterial(...)                  { return false; } // line 34
bool MaterialEditor::SaveMaterial(...)                  { return false; } // line 39
bool MaterialEditor::CompileMaterial(...)               { return false; } // line 44
uint32_t MaterialEditor::AddNode(...)                   { return 0; }    // line 49
bool MaterialEditor::ConnectSockets(...)                { return false; } // line 59
void MaterialEditor::RenderGraphEditor()                {}                // line 74
void MaterialEditor::RenderNodePalette()                {}                // line 76
void MaterialEditor::RenderMaterialPreview()            {}                // line 80
bool MaterialEditor::GenerateShaderCode(...)            { return false; } // line 109
bool MaterialEditor::ValidateMaterialGraph(...)         { return false; } // line 114
MaterialNode* MaterialEditor::CreateNode(...)           { return nullptr; }// line 98
bool MaterialEditor::SetupPreviewRendering()            { return false; } // line 142
void MaterialEditor::RenderPreviewToTexture()           {}                // line 147
```

**What is needed**: Implement node-graph editing (ImNodes or custom), HLSL code generation from graph, shader compilation integration, and preview sphere rendering via a dedicated render target.

---

### GAP-E06 — TerrainEditor is 100 % Stubbed (48 Methods)

**Files**:
- `SparkEditor/Source/Terrain/TerrainEditor.h` (695 lines)
- `SparkEditor/Source/Terrain/TerrainEditor.cpp` (194 lines)

**Impact**: No terrain can be created, sculpted, painted, or exported. The terrain pipeline is completely absent.

**Evidence**:

```cpp
// TerrainEditor.cpp
void TerrainEditor::CreateNewTerrain()                   {}                // line 85
bool TerrainEditor::LoadTerrain(...)                     { return false; } // line 87
bool TerrainEditor::SaveTerrain(...)                     { return false; } // line 92
void TerrainEditor::ApplyToolAtPosition(...)             {}                // line 97
void TerrainEditor::GenerateNoiseHeightmap(...)          {}                // line 110
void TerrainEditor::SmoothTerrain(...)                   {}                // line 115
void TerrainEditor::ApplyErosion(...)                    {}                // line 117
void TerrainEditor::UpdateTerrainMesh()                  {}                // line 126
void TerrainEditor::UpdateTerrainCollision()             {}                // line 128
float TerrainBrush::EvaluateFalloff(...)                 { return 0.0f; } // line 9
float TerrainHeightmap::GetHeight(...)                   { return 0.0f; } // line 16
float TerrainHeightmap::GetHeightInterpolated(...)       { return 0.0f; } // line 23
bool  TerrainHeightmap::LoadFromImage(...)               { return false; }// line 32
float TerrainEditor::GeneratePerlinNoise(...)            { return 0.0f; } // line 186
void  TerrainEditor::ApplyHydraulicErosion(...)          {}                // line 191
```

All 9 Render* methods (lines 132-148) and all tool application methods are empty.

**What is needed**: Implement heightmap data structure with GPU-backed vertex buffer updates, brush-based sculpting (raise/lower/smooth/flatten), multi-layer texture painting with splatmaps, Perlin noise generation, hydraulic erosion simulation, and collision mesh generation.

---

### GAP-E07 — LevelStreamingSystem is 100 % Stubbed (50 Methods)

**Files**:
- `SparkEditor/Source/LevelStreaming/LevelStreamingSystem.h` (678 lines)
- `SparkEditor/Source/LevelStreaming/LevelStreamingSystem.cpp` (230 lines)

**Impact**: No open-world tile-based streaming. Worlds cannot be created, saved, loaded, or have tiles streamed in/out. Memory management and LOD transitions are absent.

**Evidence**:

```cpp
// LevelStreamingSystem.cpp
bool LevelStreamingSystem::LoadWorld(...)               { return false; }  // line 74
bool LevelStreamingSystem::SaveWorld(...)               { return false; }  // line 79
bool LevelStreamingSystem::AddTile(...)                 { return false; }  // line 84
bool LevelStreamingSystem::RequestTileLoad(...)         { return false; }  // line 113
bool LevelStreamingSystem::ForceLoadTile(...)           { return false; }  // line 123
vector LevelStreamingSystem::GetTilesWithinDistance(...){ return {}; }     // line 133
vector LevelStreamingSystem::GetVisibleTiles(...)       { return {}; }     // line 139
int  LevelStreamingSystem::GenerateTileGridFromHeightmap(){ return 0; }   // line 154
bool LevelStreamingSystem::ValidateWorld(...)           { return false; }  // line 165
bool LevelStreamingSystem::LoadTileSync(...)            { return false; }  // line 202
bool LevelStreamingSystem::UnloadTileSync(...)          { return false; }  // line 207
void LevelStreamingSystem::BackgroundLoadingFunction()  {}                 // line 200
void LevelStreamingSystem::ProcessLoadingQueue()        {}                 // line 196
void LevelStreamingSystem::ProcessUnloadingQueue()      {}                 // line 198
void LevelStreamingSystem::UpdateMemoryManagement()     {}                 // line 192
```

Helper classes also stubbed: `WorldTile::ContainsPoint` returns false (line 9), `StreamingViewer::IsInViewFrustum` returns false (line 43).

**What is needed**: Implement tile-based world I/O (serialize/deserialize tile data), distance-based and frustum-based streaming with priority queues, background loading via worker threads, memory budget enforcement, and LOD transitions.

---

## Major Gaps

### GAP-E08 — LightingTools is 100 % Stubbed (33 Methods)

**Files**:
- `SparkEditor/Source/Lighting/LightingTools.h` (549 lines)
- `SparkEditor/Source/Lighting/LightingTools.cpp` (155 lines)

**Impact**: No lights can be created or configured in the editor. Lightmap baking, light probes, atmosphere/sky settings, and post-processing configuration are all absent.

**Evidence**:

```cpp
// LightingTools.cpp
bool LightingTools::Initialize()                       { return false; } // line 14
uint32_t LightingTools::CreateLight(...)               { return 0; }    // line 27
LightData* LightingTools::GetLight(...)                { return nullptr; }// line 36
bool LightingTools::BakeLightmaps(...)                 { return false; } // line 57
uint32_t LightingTools::GenerateLightProbes(...)       { return 0; }    // line 62
bool LightingTools::SaveLightingProfile(...)           { return false; } // line 98
bool LightingTools::LoadLightingProfile(...)           { return false; } // line 103
void LightingTools::ApplyLightingPreset(...)           {}                // line 113
void LightingTools::OptimizeLightingPerformance()      {}                // line 122
```

All 7 `Render*UI` methods (lines 126-138) are empty. No light data storage exists.

**What is needed**: Implement light data storage (vector of `LightData` structs), CRUD operations, UI for property editing, and integration with the graphics engine's light uniform buffers.

---

### GAP-E09 — InspectorPanel Uses Static Variables Instead of Entity Data

**Files**:
- `SparkEditor/Source/Panels/InspectorPanel.h`
- `SparkEditor/Source/Panels/InspectorPanel.cpp` (315 lines)

**Impact**: Property edits in the Inspector are cosmetic — they modify static local variables that are shared across all entities and disconnected from any actual ECS component data. Selecting a different entity does not update the displayed values.

**Evidence**:

```cpp
// InspectorPanel.cpp:236-238
static float position[3] = {0.0f, 0.0f, 0.0f};
static float rotation[3] = {0.0f, 0.0f, 0.0f};
static float scale[3] = {1.0f, 1.0f, 1.0f};
```

These `static` locals persist across frames and across entity selections. Every entity appears to have position `(0,0,0)`, rotation `(0,0,0)`, scale `(1,1,1)` regardless of actual data.

**Additional sub-gaps**:
- No integration with `CommandHistory` — property changes are not undoable.
- Component list in `RenderAddComponentMenu()` (line 248) is hardcoded — selecting a menu item (e.g. "Mesh Renderer") just sets `m_showAddComponentMenu = false` without actually adding a component.
- No ECS/EnTT query to read/write component data.

**What is needed**: Replace static locals with reads/writes to the selected entity's actual ECS components. Push property changes through `CommandHistory` for undo/redo. Dynamically discover available component types.

---

### GAP-E10 — GameViewPanel Renders Simulated HUD, Not Actual Game

**Files**:
- `SparkEditor/Source/Panels/GameViewPanel.h`
- `SparkEditor/Source/Panels/GameViewPanel.cpp` (929 lines)

**Impact**: The Game View shows an elaborate fake FPS HUD (crosshair, health bars, minimap, kill feed, damage indicators, ammo counter, scoreboard) but no actual game frame is rendered. All values are hardcoded or use `static` simulation variables.

**Evidence**: The 929-line implementation is entirely self-contained ImGui drawing with:
- Hardcoded health (`100`), ammo (`30/120`), armor values
- Simulated kill feed entries
- Simulated damage direction indicators
- Fake minimap with random blips
- No integration with any camera, renderer, or game state

**What is needed**: Render the game camera's perspective into an off-screen render target and present it via `ImGui::Image`. Overlay real HUD elements driven by actual game state when in Play mode.

---

### GAP-E11 — No Undo/Redo Integration Anywhere

**Files**:
- `SparkEditor/Source/CommandHistory.h` (362 lines) — well-implemented command pattern
- All panel files — zero integration

**Impact**: Despite having a complete `CommandHistory` implementation with `ICommand`, `PropertyCommand<T>`, `CompoundCommand`, and `LambdaCommand`, no editor action uses it. Object transforms, creation, deletion, reparenting, renaming — all are fire-and-forget.

**Evidence**: Grep for `CommandHistory` usage across all panel `.cpp` files returns zero hits outside of `CommandHistory.h` itself. Specific examples:

- `HierarchyPanel.cpp` — `CreateObject`, `DeleteObject`, `RenameObject`, `ReparentObject` modify scene data directly with no command wrapping.
- `InspectorPanel.cpp` — property edits go to static locals (GAP-E09), not through commands.
- `SceneViewPanel.cpp` — no transform manipulation to wrap.

**What is needed**: Create concrete command classes (`TransformCommand`, `CreateEntityCommand`, `DeleteEntityCommand`, `ReparentCommand`, `RenameCommand`, `PropertyChangeCommand`) and ensure every user-initiated mutation goes through `CommandHistory::Execute()`.

---

### GAP-E12 — VersionControlSystem is ~85 % Stubbed

**Files**:
- `SparkEditor/Source/VersionControl/VersionControlSystem.h` (781 lines)
- `SparkEditor/Source/VersionControl/VersionControlSystem.cpp` (64.4 KB)

**Impact**: Despite an extensive architecture with Git, Perforce, and SVN provider abstractions, merge handlers for `.sparkscene` and `.sparkmaterial`, and branch management UI, no actual VCS commands are executed.

**Evidence**:

```cpp
// VersionControlSystem.cpp (representative stubs)
bool VersionControlSystem::Initialize()       { return true; }  // stub
bool VersionControlSystem::Commit(...)        { return {}; }    // empty result
bool VersionControlSystem::Push(...)          { return {}; }    // empty result
bool VersionControlSystem::Pull(...)          { return {}; }    // empty result
bool VersionControlSystem::MergeBranch(...)   { return {}; }    // empty result
```

**Partially real**: `SceneMergeHandler::GetSupportedExtensions()` returns `{".sparkscene"}` (line 28), `CanMerge()` has real extension checking (line 33). Serialization helpers appear functional.

**What is needed**: Implement `GitProvider` that shells out to `git` (or uses libgit2) for status, add, commit, push, pull, diff, log, branch operations. Wire up UI to display real file status.

---

### GAP-E13 — SceneSerializer Has Extensive Header but Minimal Implementation

**Files**:
- `SparkEditor/Source/SceneSystem/SceneSerializer.h` (331 lines)
- `SparkEditor/Source/SceneSystem/SceneSerializer.cpp` (56.1 KB)

**Impact**: Scene persistence is the foundation of any editor. The header declares `SaveBinary`, `SaveJSON`, `LoadBinary`, `LoadJSON`, `CompressData`, `DecompressData`, and format-specific serializers for every component type — but the `.cpp` is architecturally large without meaningful logic in the core serialization paths.

**Related**: `SceneManager.cpp` methods `CreateNewScene`, `LoadScene`, `SaveScene` contain console output but no actual I/O logic, which compounds this gap since `SceneSerializer` is the intended serialization backend.

**What is needed**: Implement JSON serialization via nlohmann/json (already used elsewhere in the project) and binary serialization with versioned headers. Support all component types registered in the ECS.

---

## Moderate Gaps

### GAP-E14 — SceneViewPanel Camera is Orbit-Only, No FPS Fly Camera

**File**: `SparkEditor/Source/Panels/SceneViewPanel.cpp`

**Impact**: For an FPS-focused engine editor, the inability to fly through the scene with WASD+mouse is a significant workflow limitation.

**Evidence**:

```cpp
// SceneViewPanel.cpp:394
m_cameraDistance -= (dx + dy + dz) * 0.1f;
```

WASD keys (mapped to `dx`, `dy`, `dz`) modify `m_cameraDistance` — the orbit radius — instead of translating the camera position in world space. There is no forward/strafe/up movement vector computation.

**What is needed**: Implement an FPS-style fly camera: accumulate yaw/pitch from mouse delta, compute forward/right/up vectors, translate by WASD input scaled by deltaTime and a configurable speed. Keep orbit mode as an alternative (toggle with Alt key, matching industry convention).

---

### GAP-E15 — Play Mode Declared but Not Functional

**File**: `SparkEditor/Source/Core/EditorUI.h`

**Impact**: The editor declares `PlayMode` enum (`Stopped`, `Playing`, `Paused`) and toolbar buttons for Play/Pause/Stop, but pressing them does not start or stop any game simulation.

**Evidence**: `PlayMode` enum and `m_playMode` member exist in `EditorUI.h`. The toolbar renders play/pause/stop icons. However:
- No scene state snapshot is taken before entering Play mode.
- No game loop / ECS tick is started.
- No scene state restore occurs on Stop.

**What is needed**: Implement Play mode flow: serialize current scene state → start ECS simulation ticking → render game camera to GameViewPanel → on Stop, deserialize saved state to revert changes.

---

### GAP-E16 — VisualScriptingSystem is ~40 % Implemented

**Files**:
- `SparkEditor/Source/VisualScripting/VisualScriptingSystem.h` (861 lines)
- `SparkEditor/Source/VisualScripting/VisualScriptingSystem.cpp` (67.6 KB)

**Impact**: Type conversion helpers (`ToFloat`, `ToInt`, `ToBool`) are implemented, and data structures (`ScriptNode`, `ScriptGraph`, `ScriptExecutionContext`, `ScriptExecutor`) are defined. However, graph evaluation / node execution logic is largely absent.

**What is needed**: Implement the node execution engine — topological sort of the graph, per-node `Execute()` dispatch, data flow between sockets, and integration with the ECS for reading/writing component values.

---

### GAP-E17 — EngineInterface is ~40 % Simulated

**Files**:
- `SparkEditor/Source/Communication/EngineInterface.h`
- `SparkEditor/Source/Communication/EngineInterface.cpp` (576 lines)

**Impact**: The editor-engine communication layer initializes system info with hardcoded defaults and simulates metrics (e.g., `m_currentMetrics.fps = 60.0f` at line 49). Named pipe creation code exists for Windows but actual bidirectional message passing is incomplete.

**Evidence**:

```cpp
// EngineInterface.cpp:49-50
m_currentMetrics.fps = 60.0f;
m_currentMetrics.frameTime = 16.67f;
```

**What is needed**: Complete the named-pipe (Windows) and Unix domain socket (Linux) transport. Implement a message protocol (command/response with sequence IDs). Replace hardcoded metrics with real values polled from the engine process.

---

### GAP-E18 — EditorPanel Base Class SaveState/LoadState Return Defaults

**File**: `SparkEditor/Source/Core/EditorPanel.cpp` (77 lines)

**Impact**: No panel can persist its settings between sessions. `SaveState()` returns `"{}"` and `LoadState()` returns `true` without reading anything.

**Downstream effect**: Combined with GAP-E04 (LayoutManager stubbed), the editor has zero persistence of user workspace customization.

**What is needed**: Implement per-panel state serialization (each panel overrides `SaveState`/`LoadState` with its specific settings). Store in a JSON config file under the project directory.

---

### GAP-E19 — AssetBrowserPanel Has No Asset Processing or Thumbnails

**Files**:
- `SparkEditor/Source/Panels/AssetBrowserPanel.h`
- `SparkEditor/Source/Panels/AssetBrowserPanel.cpp` (453 lines)

**Impact**: The Asset Browser can list files and copy them into the project (`ImportAsset` at line 406), but cannot generate thumbnails, preview assets, or trigger the asset pipeline.

**Evidence**: Icons are determined purely by file extension (`GetFileTypeIcon` at line 36). No image decoding or GPU thumbnail rendering occurs. This ties directly to GAP-E02 (AdvancedAssetPipeline is stubbed).

**What is needed**: For texture assets, decode headers (stb_image) and render thumbnails to small textures. For meshes, render a preview using a simple shader. Cache thumbnails to disk for performance.

---

### GAP-E20 — HierarchyPanel Has No Undo/Redo for Mutations

**File**: `SparkEditor/Source/Panels/HierarchyPanel.cpp` (629 lines)

**Impact**: Creating, deleting, renaming, and reparenting objects in the hierarchy are irreversible. This is the panel with the most real functionality — it integrates with `SceneFile`, supports drag-and-drop, and has search — but all mutations bypass `CommandHistory`.

**What is needed**: Wrap `CreateObject`, `DeleteObject`, `RenameObject`, and `ReparentObject` calls in `ICommand` implementations that store pre/post state for undo.

---

## Minor Gaps

### GAP-E21 — OnShutdownRequested Has Incomplete Unsaved-Changes Dialog

**File**: `SparkEditor/Source/Core/EditorApplication.cpp` (lines 811-821)

**Impact**: The shutdown flow checks for unsaved changes but the dialog handling is incomplete — it may not properly block shutdown or offer save-before-exit.

**What is needed**: Implement a modal confirmation dialog ("Save / Don't Save / Cancel") that integrates with the scene serializer to persist unsaved work.

---

### GAP-E22 — SimpleHierarchyPanel Uses Local Storage, Not Scene Integration

**File**: `SparkEditor/Source/Panels/SimpleHierarchyPanel.cpp` (234 lines)

**Impact**: `SimpleHierarchyPanel` maintains its own `std::vector` of objects independently of any scene file. Changes made here are not reflected in `SceneFile` or the main `HierarchyPanel`.

**What is needed**: Either remove this panel (it duplicates `HierarchyPanel`) or refactor it to read from `SceneFile`.

---

### GAP-E23 — EditorCrashHandler Linux Signal Handling is Basic

**File**: `SparkEditor/Source/Core/EditorCrashHandler.cpp` (~954 lines)

**Impact**: Windows crash handling (SEH, minidumps) is thorough. Linux signal handling catches SIGSEGV/SIGABRT but does not generate core dumps programmatically or capture stack traces via `backtrace()`.

**What is needed**: On Linux, use `backtrace()` / `backtrace_symbols()` to capture and log call stacks before the crash dialog.

---

### GAP-E24 — PerformanceProfiler Header is Extensive but GPU Profiling Uncertain

**Files**:
- `SparkEditor/Source/Profiler/PerformanceProfiler.h` (685 lines)
- `SparkEditor/Source/Profiler/PerformanceProfiler.cpp` (52.5 KB)

**Impact**: CPU-side profiling (`PerformanceCounter::AddSample`, `GetSmoothedValue`) appears to have real implementations (~80% real). However, GPU query timing (D3D11 timestamp queries) integration is declared in the header but confirmation of real implementation in the large `.cpp` is needed.

**What is needed**: Verify GPU profiling uses actual D3D11 timestamp/disjoint queries. If not, implement them for per-pass and per-draw-call GPU timing.

---

### GAP-E25 — SceneFile Lookups Are O(n) Linear Scans

**File**: `SparkEditor/Source/SceneSystem/SceneFile.cpp` (139 lines)

**Impact**: `FindObject` (line 50) and `FindObjectsByName` (line 62) do linear searches through the object list. For large scenes this will become a performance bottleneck.

**What is needed**: Add `std::unordered_map<uint32_t, size_t>` index for ID lookups and optionally a name-to-indices multimap for name searches.

---

### GAP-E26 — WeaponEditorPanel and FPSToolsPanel Have Placeholder 3D Previews

**Files**:
- `SparkEditor/Source/Panels/WeaponEditorPanel.cpp` (408 lines) — line 380: `// Placeholder for 3D weapon preview`, line 394: `// Rotating weapon silhouette (placeholder)`
- `SparkEditor/Source/Panels/FPSToolsPanel.cpp` (629 lines)

**Impact**: These FPS-specific panels are functionally complete for data editing (weapon stats, spawn points, damage calculations) but cannot preview assets visually in 3D.

**What is needed**: Render weapon meshes into small preview render targets and display via `ImGui::Image`.

---

### GAP-E27 — AnimationTimeline Has Stub Engine Integration

**File**: `SparkEditor/Source/Animation/AnimationTimeline.cpp` (line 1765)

**Evidence**:
```cpp
// This is a stub integration point that the engine's runtime would fill in.
```

**Impact**: The animation timeline UI exists but cannot drive actual skeletal animation playback in the engine.

**What is needed**: Connect keyframe evaluation output to the engine's `AnimationSystem` for real-time preview.

---

## Summary Table

| ID | Severity | Subsystem | Stub % | Methods Affected | Blocks |
|---|---|---|---|---|---|
| GAP-E01 | Critical | GizmoSystem | 100% | 22 | Scene editing |
| GAP-E02 | Critical | AdvancedAssetPipeline | 100% | 57 | Asset import/processing |
| GAP-E03 | Critical | SceneViewPanel | ~90% | N/A (rendering) | 3D viewport |
| GAP-E04 | Critical | EditorLayoutManager | 100% | 14 | Layout persistence |
| GAP-E05 | Critical | MaterialEditor | 100% | 33 | Material authoring |
| GAP-E06 | Critical | TerrainEditor | 100% | 48 | Terrain creation |
| GAP-E07 | Critical | LevelStreamingSystem | 100% | 50 | Open-world editing |
| GAP-E08 | Major | LightingTools | 100% | 33 | Light placement |
| GAP-E09 | Major | InspectorPanel | N/A | 3 (static vars) | Property editing |
| GAP-E10 | Major | GameViewPanel | ~95% | N/A (simulated) | Game preview |
| GAP-E11 | Major | CommandHistory integration | 0% usage | All mutations | Undo/Redo |
| GAP-E12 | Major | VersionControlSystem | ~85% | Core VCS ops | Source control |
| GAP-E13 | Major | SceneSerializer | ~80% | Serialize/Deserialize | Scene I/O |
| GAP-E14 | Moderate | SceneViewPanel camera | N/A | Camera control | Navigation |
| GAP-E15 | Moderate | Play Mode | N/A | Play/Pause/Stop | Game testing |
| GAP-E16 | Moderate | VisualScriptingSystem | ~60% | Node execution | Visual scripting |
| GAP-E17 | Moderate | EngineInterface | ~40% | Message passing | Editor↔Engine |
| GAP-E18 | Moderate | EditorPanel base | N/A | SaveState/LoadState | Panel persistence |
| GAP-E19 | Moderate | AssetBrowserPanel | N/A | Thumbnails/preview | Asset browsing |
| GAP-E20 | Moderate | HierarchyPanel | N/A | All mutations | Hierarchy undo |
| GAP-E21 | Minor | EditorApplication | N/A | OnShutdownRequested | Graceful exit |
| GAP-E22 | Minor | SimpleHierarchyPanel | N/A | Scene integration | Data consistency |
| GAP-E23 | Minor | EditorCrashHandler | N/A | Linux backtrace | Crash diagnosis |
| GAP-E24 | Minor | PerformanceProfiler | ~20% | GPU queries | GPU profiling |
| GAP-E25 | Minor | SceneFile | N/A | Find methods | Large scene perf |
| GAP-E26 | Minor | Weapon/FPS panels | N/A | 3D preview | Visual preview |
| GAP-E27 | Minor | AnimationTimeline | N/A | Engine integration | Anim preview |

---

## Aggregate Statistics

| Metric | Value |
|---|---|
| Total gaps identified | 27 |
| Critical | 7 |
| Major | 6 |
| Moderate | 7 |
| Minor | 7 |
| Fully stubbed subsystems (100%) | 6 (Gizmo, AssetPipeline, LayoutManager, Material, Terrain, LevelStreaming) |
| Total stub methods across all subsystems | ~290+ |
| Panels with real, working UI | 7 (Console, SimpleConsole, WeaponEditor, FPSTools, ProjectBrowser, AssetBrowser, HierarchyPanel) |
| Panels with fake/simulated content | 3 (SceneView, GameView, Inspector) |
| Subsystems with partial implementation | 4 (VisualScripting ~40%, EngineInterface ~60%, VersionControl ~15%, PerformanceProfiler ~80%) |

---

## Recommended Priority Order

1. **GAP-E01 + GAP-E03** — GizmoSystem + SceneView rendering (unblocks all spatial editing)
2. **GAP-E09 + GAP-E11** — Inspector entity binding + Undo/Redo (unblocks property editing)
3. **GAP-E13** — SceneSerializer (unblocks save/load)
4. **GAP-E04 + GAP-E18** — Layout persistence (quality of life)
5. **GAP-E02 + GAP-E19** — Asset pipeline + thumbnails (unblocks content workflow)
6. **GAP-E05 + GAP-E08** — Material + Lighting editors (unblocks art workflow)
7. **GAP-E15 + GAP-E10** — Play mode + real game rendering (unblocks testing)
8. **GAP-E06 + GAP-E07** — Terrain + Level streaming (unblocks large worlds)
9. **GAP-E12** — Version control (team workflow)
10. Everything else
