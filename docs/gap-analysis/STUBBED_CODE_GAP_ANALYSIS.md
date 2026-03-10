# SparkEngine Stubbed Code — Gap Analysis

> **Scope**: All `.cpp` and `.h` files across `SparkEngine/Source/`, `SparkEditor/Source/`, `SparkGame/Source/`, `SparkConsole/src/`, `SparkShaderCompiler/src/`, `Tests/`
> **Date**: 2026-03-10
> **Methodology**: Automated codebase-wide search for TODO/FIXME/HACK/STUB/PLACEHOLDER markers, empty function bodies, `return E_NOTIMPL` patterns, `#if 0` blocks, commented-out parameter names (`/*param*/`), and `@brief Stub` annotations. Followed by manual inspection of flagged files.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

This gap analysis catalogs all stubbed, placeholder, skeleton, and unimplemented code across the entire SparkEngine codebase. It is a cross-cutting document that supplements the per-subsystem gap analyses by providing a single consolidated view of every piece of non-functional code.

**Methodology notes**:
- **196 TODO/FIXME/HACK/STUB/PLACEHOLDER markers** found across 60 files
- **212 occurrences** of "stub/placeholder/not implemented/dummy/skeleton" across 55 files
- **15 `return E_NOTIMPL`** calls in production code
- **~280+ total stub methods** identified across all subsystems

---

## Status Summary

| Gap ID | Severity | Status | Subsystem |
|--------|----------|--------|-----------|
| GAP-STUB01 | Critical | OPEN | Editor — MaterialEditor |
| GAP-STUB02 | Critical | OPEN | Editor — TerrainEditor |
| GAP-STUB03 | Critical | OPEN | Editor — LightingTools |
| GAP-STUB04 | Critical | OPEN | Editor — LevelStreamingSystem |
| GAP-STUB05 | Critical | OPEN | Editor — AdvancedAssetPipeline |
| GAP-STUB06 | Major | OPEN | Graphics — Linux Platform Stubs |
| GAP-STUB07 | Major | OPEN | Graphics — RHI Backend Stubs |
| GAP-STUB08 | Major | OPEN | Graphics — AssetPipeline Rendering |
| GAP-STUB09 | Major | OPEN | Scripting — AngelScript Stubs |
| GAP-STUB10 | Major | OPEN | Networking — NetworkManager Stub |
| GAP-STUB11 | Moderate | OPEN | Utils — SimpleConsole Linux Stubs |
| GAP-STUB12 | Moderate | OPEN | Utils — InputManager Stubs |
| GAP-STUB13 | Moderate | OPEN | Core — Platform.h Type Stubs |
| GAP-STUB14 | Moderate | OPEN | Animation — Stub Markers |
| GAP-STUB15 | Minor | OPEN | Game — Interactive Object Stubs |
| GAP-STUB16 | Minor | OPEN | Editor — GizmoSystem Stubs |
| GAP-STUB17 | Minor | OPEN | Graphics — TODO Comments |

---

## Critical Gaps

### GAP-STUB01 — MaterialEditor is entirely skeletal (22 empty methods)

**Files**:
- `SparkEditor/Source/MaterialEditor/MaterialEditor.cpp` (lines 1-149: all 22 methods are empty stubs)
- `SparkEditor/Source/MaterialEditor/MaterialEditor.h` (class declaration)

**Impact**: The Material Editor panel is advertised in the README as a feature but contains zero functional code. Users cannot create, edit, preview, or compile materials through the editor. The node graph UI does not render. Shader generation returns `false`. All 22 public and private methods are empty or return default values.

**Evidence**:
```cpp
// MaterialEditor.cpp:3 — File-level annotation
/**
 * @brief Stub implementation of MaterialEditor
 */

// MaterialEditor.cpp:74-88 — All render methods empty
void MaterialEditor::RenderGraphEditor() {}
void MaterialEditor::RenderNodePalette() {}
void MaterialEditor::RenderMaterialProperties() {}
void MaterialEditor::RenderMaterialPreview() {}
void MaterialEditor::RenderCompilationOutput() {}
void MaterialEditor::RenderNode(MaterialNode* /*node*/) {}
void MaterialEditor::RenderNodeSockets(MaterialNode* /*node*/) {}
void MaterialEditor::RenderConnections() {}

// MaterialEditor.cpp:98-101 — Core functionality returns nullptr/false
std::unique_ptr<MaterialNode> MaterialEditor::CreateNode(MaterialNodeType /*nodeType*/)
{
    return nullptr;
}

bool MaterialEditor::GenerateShaderCode(std::string& /*outVertexShader*/, std::string& /*outPixelShader*/)
{
    return false;
}
```

**What is needed**:
1. Implement node graph rendering using imnodes (already a dependency)
2. Implement material node creation with standard PBR node types (Color, Texture, Normal, Roughness, Metallic, etc.)
3. Implement socket connection logic and graph validation
4. Implement HLSL shader code generation from the material graph
5. Implement material preview rendering using a preview render target
6. Implement material serialization (load/save)
7. Implement material compilation pipeline connecting to `Shader.cpp`

**Implementation Status**: OPEN

---

### GAP-STUB02 — TerrainEditor is entirely skeletal (30+ empty methods)

**Files**:
- `SparkEditor/Source/Terrain/TerrainEditor.cpp` (193 lines: all methods return defaults)
- `SparkEditor/Source/Terrain/TerrainEditor.h` (694 lines: full class hierarchy defined)

**Impact**: The terrain editing system has extensive class declarations (TerrainBrush, TerrainHeightmap, TerrainData, TerrainEditor) but zero functional implementations. Users cannot create, sculpt, paint, or import terrain through the editor. The header is 694 lines of detailed API; the implementation is 193 lines of stubs.

**Evidence**:
```cpp
// TerrainEditor.cpp:9-12 — Core brush falloff returns zero
float TerrainBrush::EvaluateFalloff(float /*distance*/) const
{
    return 0.0f;
}

// TerrainEditor.cpp:16-19 — Heightmap returns zero
float TerrainHeightmap::GetHeight(int /*x*/, int /*y*/) const
{
    return 0.0f;
}

// TerrainEditor.cpp:28-30 — Resize/Generate are no-ops
void TerrainHeightmap::Resize(int /*newWidth*/, int /*newHeight*/, bool /*preserveData*/) {}
void TerrainHeightmap::Generate(std::function<float(int, int)> /*generator*/) {}
```

**What is needed**:
1. Implement heightmap data storage and access (GetHeight/SetHeight with bounds checking)
2. Implement brush evaluation with falloff curves (linear, smooth, sharp)
3. Implement sculpt/raise/lower/smooth/flatten/erode terrain operations
4. Implement texture splatmap painting with layer blending
5. Implement heightmap import/export (PNG, RAW)
6. Implement terrain mesh generation and collision update
7. Implement undo/redo for terrain operations
8. Connect to the engine's existing `TerrainSystem` in `SparkEngine/Source/Game/`

**Implementation Status**: OPEN

---

### GAP-STUB03 — LightingTools is entirely skeletal (28 empty methods)

**Files**:
- `SparkEditor/Source/Lighting/LightingTools.cpp` (154 lines: all methods empty)
- `SparkEditor/Source/Lighting/LightingTools.h` (550 lines: full API defined)

**Impact**: The editor lighting tools panel cannot create, modify, or delete lights. Global illumination settings cannot be applied. Lightmap baking does nothing. Atmosphere and post-processing controls are non-functional. The header defines a comprehensive API (550 lines) with zero functional implementation (154 lines of stubs).

**Evidence**:
```cpp
// LightingTools.cpp:14-17 — Initialize always fails
bool LightingTools::Initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/)
{
    return false;
}

// LightingTools.cpp:32-34 — Light manipulation is no-op
void LightingTools::UpdateLight(uint32_t /*lightId*/, const SparkLightData& /*lightData*/) {}
void LightingTools::DeleteLight(uint32_t /*lightId*/) {}

// LightingTools.cpp:57-60 — Lightmap baking returns false
bool LightingTools::BakeLightmaps(LightBakeProgressCallback /*progressCallback*/)
{
    return false;
}
```

**What is needed**:
1. Implement light creation/update/deletion with proper scene graph integration
2. Implement light gizmo rendering (point light sphere, spot light cone, directional arrow)
3. Implement global illumination settings UI and application
4. Implement atmosphere/sky rendering controls (connect to existing `SkyAtmosphere`)
5. Implement post-processing parameter controls (connect to existing `PostProcessingSystem`)
6. Implement lightmap baking (at minimum, simple ambient occlusion)
7. Connect to engine `LightingSystem` and `LightManager`

**Implementation Status**: OPEN

---

### GAP-STUB04 — LevelStreamingSystem is entirely skeletal (25 empty methods)

**Files**:
- `SparkEditor/Source/LevelStreaming/LevelStreamingSystem.cpp` (229 lines: all methods empty)
- `SparkEditor/Source/LevelStreaming/LevelStreamingSystem.h` (class declaration)

**Impact**: Level streaming, world tiles, streaming volumes, and LOD-based loading are all non-functional. The system cannot partition worlds into tiles, cannot load/unload tiles based on player distance, and cannot manage memory for large worlds.

**Evidence**:
```cpp
// LevelStreamingSystem.cpp:9-12 — Spatial queries return false/zero
bool WorldTile::ContainsPoint(const XMFLOAT3& /*point*/) const
{
    return false;
}

float WorldTile::GetDistanceToCenter(const XMFLOAT3& /*point*/) const
{
    return 0.0f;
}

// LevelStreamingSystem.cpp:59 — Update is empty (the core streaming loop)
void LevelStreamingSystem::Update(float /*deltaTime*/) {}
```

**What is needed**:
1. Implement world tile spatial queries (bounds containment, distance calculations)
2. Implement streaming volume trigger detection
3. Implement tile load/unload queue processing with background thread loading
4. Implement distance-based, trigger-based, and predictive streaming strategies
5. Implement memory budget management for loaded tiles
6. Implement LOD level calculation based on viewer distance
7. Implement editor UI for defining streaming volumes and tile boundaries

**Implementation Status**: OPEN

---

### GAP-STUB05 — AdvancedAssetPipeline is entirely skeletal (18 empty methods)

**Files**:
- `SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.cpp` (293 lines: all methods empty)
- `SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.h` (class declaration)

**Impact**: The advanced asset pipeline cannot register asset processors, cannot monitor the filesystem for changes, cannot track asset dependencies, and cannot process assets in background threads. The dependency graph is completely non-functional.

**Evidence**:
```cpp
// All processing and monitoring methods are empty stubs
void AdvancedAssetPipeline::ProcessingThreadFunction() {}
void AdvancedAssetPipeline::FileSystemMonitoringFunction() {}
void AdvancedAssetPipeline::UpdateDependencyGraph() {}
```

**What is needed**:
1. Implement asset processor registration with type-based dispatch
2. Implement filesystem monitoring (inotify on Linux, ReadDirectoryChangesW on Windows)
3. Implement dependency graph tracking and dirty propagation
4. Implement background thread asset processing with progress reporting
5. Implement import settings serialization per asset type
6. Implement editor UI panels for asset browser and import settings

**Implementation Status**: OPEN

---

## Major Gaps

### GAP-STUB06 — Graphics Engine Linux platform stubs (~60 empty methods)

**Files**:
- `SparkEngine/Source/Graphics/GraphicsEngine.cpp` (lines 3720-3800: 12 methods return `E_NOTIMPL`)
- `SparkEngine/Source/Graphics/PostProcessingSystem.cpp` (12 stub methods)
- `SparkEngine/Source/Graphics/LightingSystem.cpp` (8 stub methods)
- `SparkEngine/Source/Graphics/TextureSystem.cpp` (stub class)
- `SparkEngine/Source/Graphics/RenderTarget.cpp` (stub class)
- `SparkEngine/Source/Graphics/ParticleSystem.cpp` (stub class)
- `SparkEngine/Source/Graphics/MeshLOD.cpp` (stub class)
- `SparkEngine/Source/Graphics/DecalSystem.cpp` (stub class)
- `SparkEngine/Source/Graphics/MaterialSystem.cpp` (6 stub methods)
- `SparkEngine/Source/Graphics/Shader.cpp` (line 756: `return E_NOTIMPL`)

**Impact**: The entire graphics subsystem is non-functional on Linux. All render paths (forward, deferred, forward+), all post-processing effects, particle systems, mesh LOD, decals, lighting, and material binding are no-ops. The engine compiles on Linux but renders nothing.

**Evidence**:
```cpp
// GraphicsEngine.cpp:3721-3748 — Device creation returns E_NOTIMPL on Linux
HRESULT GraphicsEngine::CreateDeviceAndSwapChain(HWND) { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateDevice(HWND, uint32_t, uint32_t, bool) { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateRenderTargetView() { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateDepthStencilView() { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateRenderTargets() { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateAdvancedRenderTargets() { return E_NOTIMPL; }
HRESULT GraphicsEngine::CreateRenderStates() { return E_NOTIMPL; }
void GraphicsEngine::SetViewport() {}
```

**What is needed**:
1. Implement OpenGL 4.5 or Vulkan backend through the existing RHI abstraction layer
2. Implement platform-agnostic render path selection
3. Port all post-processing effects to shader-backend-agnostic implementations
4. Implement cross-platform texture loading, render target management, and material binding
5. Ensure all GLSL shaders in `Shaders/GLSL/` are functional equivalents of HLSL counterparts

**Implementation Status**: OPEN

---

### GAP-STUB07 — RHI Backend stubs (Vulkan: 7, OpenGL: 4, D3D11: 6)

**Files**:
- `SparkEngine/Source/Graphics/RHI/` (VulkanDevice, OpenGLDevice, D3D11Device)

**Impact**: The RHI abstraction layer has incomplete backend implementations. Vulkan is missing descriptor binding (`SetConstantBuffer`, `SetShaderResource`, `SetSampler`), OpenGL is missing lifecycle management (`Begin`, `Reset`, `ExecuteCommandList`), and D3D11 is missing debug annotations and command list execution.

**Evidence**:
```cpp
// VulkanDevice — Missing descriptor binding
void VulkanDevice::SetPrimitiveTopology(...) {}
void VulkanDevice::SetConstantBuffer(...) {}
void VulkanDevice::SetShaderResource(...) {}
void VulkanDevice::SetSampler(...) {}

// OpenGLDevice — Missing lifecycle
void OpenGLDevice::Begin() {}
void OpenGLDevice::Reset() {}
void OpenGLDevice::ExecuteCommandList() {}
void OpenGLDevice::EndFrame() {}
```

**What is needed**:
1. Implement Vulkan descriptor set binding for constant buffers, SRVs, and samplers
2. Implement OpenGL state management lifecycle
3. Implement D3D11 deferred context command list execution
4. Add debug annotation support across all backends

**Implementation Status**: OPEN

---

### GAP-STUB08 — AssetPipeline rendering helpers are stubs

**Files**:
- `SparkEngine/Source/Graphics/AssetPipeline.cpp` (lines 2449-2462: 3 TODO stubs)
- `SparkEngine/Source/Graphics/AssetPipeline.cpp` (lines 2343-2350: 2 `return E_NOTIMPL`)

**Impact**: The asset pipeline's render integration (`BindMesh`, `BindMaterial`, `DrawBoundMesh`) does nothing. Assets can be loaded but cannot be rendered through the pipeline. FBX and glTF loading returns `E_NOTIMPL` on non-Windows platforms.

**Evidence**:
```cpp
// AssetPipeline.cpp:2449-2462 — Rendering helpers are empty
void AssetPipeline::BindMesh([[maybe_unused]] const std::string& meshPath)
{
    // TODO: Bind mesh vertex/index buffers to the pipeline
}

void AssetPipeline::BindMaterial([[maybe_unused]] const std::string& materialPath)
{
    // TODO: Bind material textures and constants to the pipeline
}

void AssetPipeline::DrawBoundMesh()
{
    // TODO: Issue draw call for the currently bound mesh
}
```

**What is needed**:
1. Implement `BindMesh` to look up cached mesh data and bind vertex/index buffers
2. Implement `BindMaterial` to set texture SRVs, samplers, and material constant buffer
3. Implement `DrawBoundMesh` to issue the appropriate draw call
4. Implement FBX/glTF loading on Linux via Assimp (already a dependency)

**Implementation Status**: OPEN

---

### GAP-STUB09 — AngelScript engine stubs when scripting is disabled (15 methods)

**Files**:
- `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp` (stub class when `SPARK_ANGELSCRIPT_SUPPORT` is off)
- `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h`

**Impact**: When AngelScript support is disabled at compile time, 15 scripting methods are stubbed. Additionally, `ASGetKeyDown()` at line 69 is stubbed even when AngelScript IS enabled, preventing scripts from reading keyboard input.

**Evidence**:
```cpp
// AngelScriptEngine.cpp:69 — Stub even when AngelScript is enabled
// Requires InputManager binding — stub for now.
bool ASGetKeyDown(int) { return false; }
```

**What is needed**:
1. Implement `ASGetKeyDown` by binding to `InputManager::IsKeyDown()`
2. Ensure all AngelScript engine API bindings cover the full input system
3. Consider adding stub warnings/logging when scripting is compiled out

**Implementation Status**: OPEN

---

### GAP-STUB10 — NetworkManager stub when networking is disabled

**Files**:
- `SparkEngine/Source/Engine/Networking/NetworkManager.cpp` (9 TODO markers, 15+ `#ifdef` guard blocks)
- `SparkEngine/Source/Engine/Networking/NetworkManager.h` (5 TODO markers, stub class)

**Impact**: When `ENABLE_NETWORKING=OFF` (the default), the `NetworkManagerStub` class provides empty implementations for all networking methods. `Initialize()` returns false, `StartServer()`/`Connect()` return false.

**Evidence**:
```cpp
// NetworkManagerStub — All methods return failure/no-op
bool NetworkManagerStub::Initialize() { return false; }
bool NetworkManagerStub::StartServer(uint16_t) { return false; }
bool NetworkManagerStub::Connect(const std::string&, uint16_t) { return false; }
```

**What is needed**:
1. This is intentional design (networking is opt-in). Status is expected.
2. Consider adding compile-time diagnostic messages when networking methods are called with networking disabled
3. Complete the TODO items within the networking implementation itself (9 open TODOs)

**Implementation Status**: OPEN

---

## Moderate Gaps

### GAP-STUB11 — SimpleConsole non-Windows stubs (29 empty methods)

**Files**:
- `SparkEngine/Source/Utils/SparkConsole.cpp` (line 5422+: "Non-Windows: Minimal stub implementation")

**Impact**: On Linux, the debug console has no registered commands (all 25 `Register*Commands()` methods are empty). Tab completion, history navigation, and watch expressions are non-functional. The console renders but cannot execute any commands.

**What is needed**:
1. Port console command registration to be platform-independent (most commands are not Windows-specific)
2. Implement tab completion using the registered command list
3. Implement command history navigation

**Implementation Status**: OPEN

---

### GAP-STUB12 — InputManager stubs on non-Windows

**Files**:
- `SparkEngine/Source/Input/` (5 stub methods)

**Impact**: `KeyNameToVirtualKey()` returns 0, `VirtualKeyToKeyName()` returns "Unknown". Console input simulation and input event logging are empty.

**What is needed**:
1. Implement key name mapping for Linux (X11/Wayland keysyms or SDL2 scancodes)
2. Implement input event logging for debugging

**Implementation Status**: OPEN

---

### GAP-STUB13 — Platform.h type stubs (12+ stub groups)

**Files**:
- `SparkEngine/Source/Core/Platform.h` (29 TODO/STUB markers)

**Impact**: On non-Windows platforms, minimal stubs exist for DirectXMath types, COM/ComPtr, WAVEFORMATEX, XAudio2 interfaces, D3D11 types, DXGI_FORMAT, FILETIME, and MessageBox. These allow compilation but provide no functionality.

**What is needed**:
1. Replace DirectXMath stubs with GLM-based implementations (GLM is already a dependency)
2. Replace XAudio2 stubs with miniaudio implementations (miniaudio is already a dependency)
3. Implement proper cross-platform file time, message box, and COM-like reference counting

**Implementation Status**: OPEN

---

### GAP-STUB14 — Animation system stub markers (94 occurrences)

**Files**:
- `SparkEngine/Source/Engine/Animation/AnimationSystem.cpp` (46 "stub/placeholder/dummy" markers)
- `SparkEngine/Source/Engine/Animation/AnimationSystem.h` (48 "stub/placeholder/dummy" markers)

**Impact**: The animation system has the highest density of stub markers in the codebase. While the core skeletal animation works, many advanced features (multi-layer blending masks, FABRIK convergence, root motion channels) may be incomplete or use placeholder logic.

**What is needed**:
1. Audit each of the 94 stub markers to determine which represent actual missing functionality vs. placeholder comments on working code
2. Prioritize completing any incomplete IK solvers, blend masks, and root motion extraction

**Implementation Status**: OPEN

---

## Minor Gaps

### GAP-STUB15 — Game module interactive object stubs

**Files**:
- `SparkGame/Source/Game/` (multiple game object files: CubeObject, SphereObject, WallObject, etc.)

**Impact**: Game objects have `OnHit()` and `OnHitWorld()` methods that are empty. The `GravitySystem` has a placeholder zone removal (line 94). These are example game module code and have minimal impact on the engine itself.

**What is needed**:
1. Implement collision response in example game objects (visual feedback, sound, damage)
2. Complete gravity zone removal logic

**Implementation Status**: OPEN

---

### GAP-STUB16 — GizmoSystem minimal stubs (3 methods)

**Files**:
- `SparkEditor/Source/Gizmos/GizmoSystem.cpp`

**Impact**: `Update()`, `Shutdown()`, and `ApplyTranslation()` are empty. The gizmo system partially works through ImGuizmo but lacks custom engine-integrated transform gizmos.

**What is needed**:
1. Implement gizmo update loop with mouse intersection testing
2. Implement translation/rotation/scale apply methods
3. Connect to the ECS transform component system

**Implementation Status**: OPEN

---

### GAP-STUB17 — AssetPipeline TODO comments (3 items)

**Files**:
- `SparkEngine/Source/Graphics/AssetPipeline.cpp` (lines 2451, 2456, 2461)

**Impact**: Three explicit TODO comments mark unimplemented rendering helper functions. See GAP-STUB08 for details.

**What is needed**:
1. Address as part of GAP-STUB08 implementation

**Implementation Status**: OPEN
**See also**: GAP-STUB08

---

## Implementation Priority

Recommended implementation order based on user impact and dependency chains:

1. **GAP-STUB13** — Platform.h type stubs (prerequisite for all Linux functionality)
2. **GAP-STUB06** — Graphics Linux stubs (enables rendering on Linux)
3. **GAP-STUB07** — RHI backend stubs (enables Vulkan/OpenGL rendering)
4. **GAP-STUB08** — AssetPipeline rendering (enables asset display)
5. **GAP-STUB11** — SimpleConsole Linux stubs (enables debugging on Linux)
6. **GAP-STUB12** — InputManager stubs (enables input on Linux)
7. **GAP-STUB01** — MaterialEditor (most impactful editor feature)
8. **GAP-STUB02** — TerrainEditor (large scope, high value)
9. **GAP-STUB03** — LightingTools (editor quality of life)
10. **GAP-STUB04** — LevelStreamingSystem (large world support)
11. **GAP-STUB05** — AdvancedAssetPipeline (workflow improvement)
12. **GAP-STUB09** — AngelScript input binding (scripting completeness)
13. **GAP-STUB14** — Animation stub audit (identify real gaps)
14. **GAP-STUB10** — Networking TODO items (when networking is prioritized)
15. **GAP-STUB15** — Game module stubs (example code, lowest priority)
16. **GAP-STUB16** — GizmoSystem (editor polish)

---

## Architectural Recommendations

1. **Linux platform parity should follow the RHI abstraction layer** — Rather than porting D3D11 calls directly, invest in completing the Vulkan and OpenGL RHI backends. This provides a single path to multi-platform rendering.

2. **Editor skeleton modules should be implemented incrementally** — The five skeleton editor subsystems (MaterialEditor, TerrainEditor, LightingTools, LevelStreamingSystem, AdvancedAssetPipeline) share a common pattern: extensive headers with empty implementations. Consider implementing one fully as a reference, then using that pattern for the others.

3. **Platform.h stubs should be replaced with real abstractions** — The current approach of stubbing Windows types on Linux is fragile. Replace with proper cross-platform abstractions (GLM for math, miniaudio for audio, SDL2 for input/windowing) behind platform-agnostic interfaces.

4. **Animation stub audit is high-priority information** — With 94 stub markers, the animation system may have more real gaps than any other subsystem, but many markers may be false positives on working code. An audit should precede any implementation work.

5. **Console command registration should be platform-independent** — The vast majority of the 200+ console commands are not Windows-specific (ECS queries, scene management, gameplay tweaks). Factor out the platform-dependent commands and register the rest unconditionally.

---

## Cross-References

- `GRAPHICS_GAP_ANALYSIS.md` — Covers rendering pipeline gaps in detail
- `EDITOR_GAP_ANALYSIS.md` — Covers editor panel functionality gaps
- `ANIMATION_GAP_ANALYSIS.md` — Covers animation system gaps
- `NETWORKING_GAP_ANALYSIS.md` — Covers networking implementation gaps
- `SCRIPTING_GAP_ANALYSIS.md` — Covers scripting integration gaps
- `CORE_INFRASTRUCTURE_GAP_ANALYSIS.md` — Covers platform abstraction gaps
- `INPUT_GAP_ANALYSIS.md` — Covers input system gaps
- `STABILITY_GAP_ANALYSIS.md` — Covers cross-cutting quality issues
