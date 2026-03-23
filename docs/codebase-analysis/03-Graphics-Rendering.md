# 03 — Graphics & Rendering

**Location:** `SparkEngine/Source/Graphics/`

The Graphics subsystem provides a complete rendering pipeline with D3D11 as the primary backend, an RHI abstraction layer for cross-platform support, a declarative render graph, and specialized renderers for terrain, water, sky, shadows, and post-processing.

---

## GraphicsEngine — Master Orchestrator

**File:** `SparkEngine/Source/Graphics/GraphicsEngine.h`

Central coordinator for all rendering operations. Owns the D3D11 device, manages subsystems, processes draw lists, and orchestrates the frame.

### Key Methods

```cpp
class GraphicsEngine {
public:
    // Lifecycle
    HRESULT Initialize(HWND hwnd);
    void Shutdown();
    void Resize(int width, int height);

    // Frame
    void BeginFrame();
    void EndFrame();
    void RenderScene();

    // ECS draw submission (thread-safe via mutex)
    void SubmitMeshForRendering(const MeshDrawCommand& cmd);
    void ProcessDrawList();

    // Pipeline configuration
    void SetRenderingPipeline(RenderPipeline pipeline);
    void SetGraphicsSettings(const GraphicsSettings& settings);

    // Subsystem access
    RHIBridge* GetRHIBridge();
    IRHIDevice* GetRHIDevice();
    PipelineStateCache* GetPipelineStateCache();
    ShadowAtlas* GetShadowAtlas();

    // Console integration
    GraphicsMetrics Console_GetMetrics() const;
    void Console_SetWireframe(bool enabled);
    void Console_ReloadShaders();
};
```

### Managed Subsystems

| Subsystem | Purpose |
|-----------|---------|
| TextureSystem | Texture loading and streaming |
| MaterialSystem | PBR material management |
| LightingSystem | Light components, IBL, environment |
| LightManager | Per-frame culling, tile binning |
| ShadowAtlas | Shadow map atlas management |
| PostProcessingPipeline | HDR pipeline, effects chain |
| AssetPipeline | Model/asset loading |
| TemporalEffects | TAA, reprojection |
| ScreenSpaceEffects | SSAO, SSR, volumetrics |

### G-Buffer Layout (Deferred Path)

| RT | Format | Content |
|----|--------|---------|
| 0 | RGBA8 | Albedo + Alpha |
| 1 | RGB10A2 | World Normal |
| 2 | RGBA8 | Metallic/Roughness/AO/Material ID |
| 3 | RG16F | Motion Vectors |

### Rendering Pipelines

- **Forward**: Single pass, all lights evaluated per pixel
- **Deferred**: G-buffer pass + lighting pass
- **Forward+**: Tile-based light culling + forward shading
- **Clustered**: 3D cluster-based light assignment

---

## Shader — HLSL Compilation & Constants

**File:** `SparkEngine/Source/Graphics/Shader.h`

Manages shader compilation, variants, constant buffers, and hot-reload.

### Constant Buffer Layout

```cpp
struct PerFrameConstants {
    XMMATRIX viewMatrix;
    XMMATRIX projectionMatrix;
    XMMATRIX viewProjection;
    XMFLOAT3 cameraPosition;
    float time;
    float deltaTime;
    XMFLOAT2 screenSize;
    float nearPlane, farPlane;
};

struct PerObjectConstants {
    XMMATRIX worldMatrix;
    XMMATRIX worldViewProjection;
    XMMATRIX normalMatrix;
};

struct PerMaterialConstants {
    XMFLOAT4 albedoColor;
    float metallic, roughness, ao;
    float emissiveStrength;
};

struct LightingData {
    // Directional + point + spot light arrays
    // Shadow matrices, ambient color, IBL parameters
};
```

### Shader Variants

```cpp
ShaderVariant variant = shader.CreateShaderVariant({
    {"ENABLE_SHADOWS", "1"},
    {"QUALITY_LEVEL", "HIGH"},
    {"ENABLE_NORMAL_MAPPING", "1"}
});
shader.SetActiveVariant(variant);
```

### Hot-Reload

```cpp
shader.HotReloadShaders();  // Recompile modified shaders at runtime
// Console: shader_reload, shader_list, shader_metrics
```

---

## RHI — Rendering Hardware Interface

**Location:** `SparkEngine/Source/Graphics/RHI/`

Abstract backend layer supporting multiple graphics APIs.

### IRHIDevice — Central Abstraction

```cpp
class IRHIDevice {
public:
    // Lifecycle
    virtual bool Initialize(const RHIDeviceDesc& desc) = 0;
    virtual void Shutdown() = 0;

    // Resource creation
    virtual RHIBufferHandle CreateBuffer(const RHIBufferDesc& desc) = 0;
    virtual RHITextureHandle CreateTexture(const RHITextureDesc& desc) = 0;
    virtual RHIShaderHandle CreateShader(const RHIShaderDesc& desc) = 0;
    virtual RHISamplerHandle CreateSampler(const RHISamplerDesc& desc) = 0;
    virtual RHIPipelineStateHandle CreatePipelineState(const RHIPipelineStateDesc& desc) = 0;

    // Resource updates
    virtual void* MapBuffer(RHIBufferHandle handle) = 0;
    virtual void UnmapBuffer(RHIBufferHandle handle) = 0;
    virtual void UpdateTexture(RHITextureHandle handle, const void* data) = 0;

    // Command lists
    virtual IRHICommandList* GetImmediateCommandList() = 0;
    virtual IRHICommandList* CreateDeferredCommandList() = 0;
    virtual void ExecuteCommandList(IRHICommandList* list) = 0;

    // Frame
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void WaitForIdle() = 0;

    // Info
    virtual GraphicsBackend GetBackendType() const = 0;
    virtual RHICapabilities GetCapabilities() const = 0;
    virtual RHIStatistics GetStatistics() const = 0;
};
```

### Supported Backends

| Backend | Status | Notes |
|---------|--------|-------|
| D3D11 | Primary | Full feature support |
| D3D12 | Experimental | Basic resource management |
| Vulkan | Experimental | SPIR-V compilation |
| OpenGL | Experimental | GLAD loader |
| Metal | Experimental | macOS only |

### RHI File Structure

```
RHI/
├── RHIDevice.h         — Abstract device interface
├── RHITypes.h          — Enums, descriptors, capabilities
├── RHIResources.h      — Abstract resource types (buffer, texture, shader)
├── RHIFactory.h        — Device creation, backend selection
├── RHIBridge.h         — D3D11 bridge to RHI (wraps native resources)
├── RHIPipelineTypes.h  — Pipeline state descriptors
├── RHIShaderCompiler.h — Cross-platform shader compilation
└── Backends/
    ├── D3D11/          — Direct3D 11 implementation
    ├── D3D12/          — Direct3D 12 implementation
    ├── Vulkan/         — Vulkan implementation
    ├── OpenGL/         — OpenGL implementation
    └── Metal/          — Metal implementation (macOS)
```

---

## RenderGraph — Declarative Frame Graph

**File:** `SparkEngine/Source/Graphics/RenderGraph.h`

DAG-based declarative rendering pipeline. Passes declare resource dependencies; the graph handles ordering, resource aliasing, and dead-code elimination.

### Usage

```cpp
RenderGraph graph;

// Declare passes
auto& shadowPass = graph.AddPass("ShadowMap", RenderGraphPassType::Graphics,
    [&](RenderGraphBuilder& builder) {
        builder.CreateTexture("ShadowDepth", {2048, 2048, DXGI_FORMAT_D32_FLOAT});
        builder.WriteDepth("ShadowDepth");
    },
    [&](const RenderGraphResourceRegistry& registry) {
        // Execute shadow rendering
    });

auto& gBufferPass = graph.AddPass("GBuffer", RenderGraphPassType::Graphics,
    [&](RenderGraphBuilder& builder) {
        builder.CreateTexture("Albedo", {width, height, DXGI_FORMAT_R8G8B8A8_UNORM});
        builder.CreateTexture("Normal", {width, height, DXGI_FORMAT_R10G10B10A2_UNORM});
        builder.WriteColor("Albedo", 0);
        builder.WriteColor("Normal", 1);
        builder.ReadTexture("ShadowDepth");
    },
    [&](const RenderGraphResourceRegistry& registry) {
        // Execute G-buffer rendering
    });

// Compile: topological sort, DCE, resource aliasing
graph.Compile();

// Execute: allocate transients, run passes, release
graph.Execute();
```

### Compilation Phase

1. **Dead-Code Elimination** — Passes with no consumers are pruned
2. **Topological Sort** — Ensures correct execution order
3. **Resource Lifetime Analysis** — Track first-use to last-use
4. **Resource Aliasing** — Non-overlapping lifetimes share memory
5. **Async Compute Scheduling** — (stub) Independent passes can overlap

### Key Classes

| Class | Purpose |
|-------|---------|
| `RenderGraph` | DAG container, compile/execute |
| `RenderGraphPass` | Individual rendering work unit |
| `RenderGraphBuilder` | Pass setup, dependency declaration |
| `RenderGraphBlackboard` | Type-erased inter-pass data sharing |
| `RenderGraphResourceRegistry` | Pass-time resource handle resolution |
| `TransientResourcePool` | Pooled transient render target allocation |

---

## LightManager — Per-Frame Culling & Tiling

**File:** `SparkEngine/Source/Graphics/LightManager.h`

Forward+ compatible light culling with tile-based binning.

### Per-Frame Lifecycle

```cpp
lightManager.BeginFrame();                    // Clear submissions, extract frustum

// Submit lights (from ECS LightComponent query)
for (auto& light : lights)
    lightManager.SubmitLight(light);

lightManager.CullAndBinLights();              // Frustum cull + tile assignment

// Query results
auto& visible = lightManager.GetVisibleLights();
auto& shadows = lightManager.GetShadowCasters();
auto& tileLights = lightManager.GetTileLightList(tileX, tileY);
```

### Tile Grid

- Screen divided into tiles (configurable size, e.g., 16x16 pixels)
- Each tile stores up to 256 light indices
- Directional lights affect all tiles
- Point/spot lights assigned via screen-space AABB intersection

---

## ShadowAtlas — Priority-Based Shadow Maps

**File:** `SparkEngine/Source/Graphics/ShadowAtlas.h`

Single large depth texture subdivided into variable-size tiles for shadow maps.

```cpp
ShadowAtlas atlas;
atlas.Initialize(4096, 256);  // 4096x4096 atlas, 256px minimum tile

atlas.BeginFrame();
auto tile = atlas.RequestTile(lightId, priority, 1024);  // 1024x1024 tile
// Render shadow map into tile viewport
atlas.EndFrame();  // evict stale tiles
```

- **Priority-based allocation**: Higher priority = larger tiles
- **Temporal reuse**: Tiles persist across frames, re-rendered only when stale
- **LRU eviction**: When space exhausted, least-recently-used tiles freed

---

## PostProcessing — HDR Effect Chain

**File:** `SparkEngine/Source/Graphics/PostProcessingEffects.h`

14 post-processing effects with per-effect settings:

| Pass | Key Settings |
|------|-------------|
| Bloom | Threshold, intensity, radius, mip count |
| AutoExposure | Min/max EV, adaptation speed, key value |
| Tonemapping | Operator (ACES/Filmic/Neutral/Reinhard), exposure |
| ColorGrading | Temperature, tint, saturation, contrast, shadows/midtones/highlights |
| FXAA | Quality (Low/Medium/High/Ultra) |
| DepthOfField | Focus distance, aperture, max blur radius |
| MotionBlur | Intensity, max velocity, sample count |
| Vignette | Intensity, smoothness |
| ChromaticAberration | Intensity |
| FilmGrain | Intensity, size |
| LensDistortion | Barrel/pincushion, chromatic |
| LightShafts | Intensity, decay, weight, density |
| LensFlare | Threshold, intensity, ghost count |
| Sharpen | Strength |

---

## SkyAtmosphere — Preetham Sky Model

**File:** `SparkEngine/Source/Graphics/SkyAtmosphere.h`

Analytical sky evaluation using Perez distribution:

```cpp
auto& sky = SkyAtmosphereSystem::GetInstance();
sky.SetSunDirection({0.5f, 0.8f, 0.3f});
sky.SetTurbidity(2.5f);

SkyColor color = sky.ComputeSkyColor(viewDirection);
SkyColor sunColor = sky.ComputeSunColor();
```

CPU-side computation outputting linear HDR colors for skybox or shader input. Integrates with WeatherSystem and TimeOfDaySystem.

---

## WaterRenderer — Gerstner Wave Simulation

**File:** `SparkEngine/Source/Graphics/WaterRenderer.h`

Ocean-like water surfaces using Gerstner wave summation:

```cpp
auto& water = WaterRenderer::GetInstance();
auto planeId = water.AddWaterPlane({0, 0, 0}, {100, 100});  // center, size

water.Update(deltaTime);  // animate waves

float height = water.GetWaterHeight(worldX, worldZ);  // for buoyancy
auto& vertices = water.GetMeshVertices(planeId);       // for rendering
```

Features: peaked crests, flat troughs, configurable wave components (direction, amplitude, wavelength, speed, steepness).

---

## TerrainRenderer — ECS Terrain Rendering

**File:** `SparkEngine/Source/Graphics/TerrainRenderer.h`

Renders ECS `TerrainComponent` entities with heightmap-to-mesh generation:

```cpp
terrainRenderer.Initialize(device);
terrainRenderer.UpdateTerrains(dirtyTerrainMap);  // rebuild dirty meshes
terrainRenderer.Render(context, viewMatrix, projMatrix);
```

- Heightmap → grid mesh with normals, UVs, and 4-layer splat weights
- LOD support (selectable per entity)
- Splat mapping for multi-texture blending

---

## Additional Graphics Systems

| System | File | Purpose |
|--------|------|---------|
| TemporalEffects | `TemporalEffects.h` | TAA, temporal reprojection, jitter sequences |
| ScreenSpaceEffects | `ScreenSpaceEffects.h` | SSAO, SSR, volumetric lighting |
| VirtualTexture | `VirtualTexture.h` | Virtual texture streaming (megatexture) |
| MaterialSystem | `MaterialSystem.h` | PBR material management, property blocks |
| TextureSystem | `TextureSystem.h` | Texture loading, caching, streaming |
| AssetPipeline | `AssetPipeline.h` | Model/mesh/asset loading (OBJ, FBX via Assimp) |
| LightingSystem | `LightingSystem.h` | Light components, IBL, environment probes |

---

## Graphics File Structure

```
Graphics/
├── GraphicsEngine.h/cpp          — Master renderer
├── Shader.h/cpp                  — HLSL compilation, constant buffers
├── PostProcessingEffects.h       — Effect settings (data only)
├── PostProcessingPipeline.h/cpp  — HDR pipeline implementation
├── MaterialSystem.h/cpp          — PBR materials
├── TextureSystem.h/cpp           — Texture streaming
├── AssetPipeline.h/cpp           — Model/asset loading
├── LightingSystem.h/cpp          — Light components, IBL
├── LightManager.h/cpp            — Per-frame culling, tiling
├── ShadowAtlas.h/cpp             — Shadow atlas management
├── RenderGraph.h/cpp             — Declarative frame graph
├── RenderPipeline.h/cpp          — RenderGraph-based pipeline
├── SkyAtmosphere.h/cpp           — Preetham sky model
├── WaterRenderer.h/cpp           — Gerstner wave water
├── TerrainRenderer.h/cpp         — ECS terrain rendering
├── TemporalEffects.h/cpp         — TAA, reprojection
├── ScreenSpaceEffects.h/cpp      — SSAO, SSR
├── VirtualTexture.h/cpp          — Virtual texture streaming
├── RHI/
│   ├── RHIDevice.h               — Abstract device interface
│   ├── RHITypes.h                — Enums, descriptors
│   ├── RHIResources.h            — Abstract resource types
│   ├── RHIFactory.h              — Device creation
│   ├── RHIBridge.h               — D3D11 bridge
│   ├── RHIPipelineTypes.h        — Pipeline state descriptors
│   ├── RHIShaderCompiler.h       — Cross-platform compilation
│   └── Backends/                 — D3D11, D3D12, Vulkan, OpenGL, Metal
└── RenderGraph/
    ├── RenderGraphTypes.h        — Enums, descriptors
    ├── RenderGraphPass.h         — Pass classes
    ├── RenderGraphBuilder.h      — Dependency declaration
    ├── RenderGraphBlackboard.h   — Inter-pass data
    └── TransientResourcePool.h   — Transient RT pooling
```
