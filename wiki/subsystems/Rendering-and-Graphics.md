# Rendering and Graphics

> **Stable-v1 boundary:** `stable-v1` is blocked and uncertified. Its declared
> rendering scope is Windows 11 x64 with MSVC v143, D3D11, or the no-render
> Windows NullRHI path. D3D12, Vulkan, OpenGL, and Metal remain experimental
> and outside the profile; this page is not a six-backend support claim.

SparkEngine contains RHI implementations for D3D11, D3D12, Vulkan, OpenGL,
Metal, and NullRHI behind a Render Hardware Interface (RHI) abstraction. That
source inventory does not establish feature parity, host support, or release
certification for every backend. The `GraphicsEngine` source includes PBR
materials, several render-path implementations, a RenderGraph, GI, GPU-driven
rendering, post-processing, temporal effects, texture streaming, and console
integration; individual capabilities retain their own verification boundaries.

**Source:** `SparkEngine/Source/Graphics/`

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                          GraphicsEngine                              │
│  Initialize, BeginFrame, RenderScene, EndFrame, Shutdown             │
│                                                                      │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────┐ ┌─────────────┐  │
│  │ MaterialSys  │ │ TextureSys   │ │ LightingSys │ │ PostProcess │  │
│  │ (PBR, slots, │ │ (streaming,  │ │ (deferred,  │ │ (bloom, HDR,│  │
│  │  hot-reload) │ │  LRU evict)  │ │  shadows)   │ │  tonemap)   │  │
│  └──────────────┘ └──────────────┘ └─────────────┘ └─────────────┘  │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────┐ ┌─────────────┐  │
│  │ LightManager │ │ AssetPipeline│ │ Temporal     │ │ PostProcess │  │
│  │ (cull, tile, │ │ (mesh/model  │ │ Effects     │ │ Pipeline    │  │
│  │  shadow atlas│ │  loading)    │ │ (TAA, jitter│ │ (composable)│  │
│  └──────────────┘ └──────────────┘ └─────────────┘ └─────────────┘  │
├──────────────────────────────────────────────────────────────────────┤
│  Renderer Integration Systems                                        │
│  PipelineStateCache | RenderTargetPool | BVHAccelerator              │
│  GPUSceneBuffer | ConstantBufferRing | GPUDebugMarkers               │
│  GPUTimestampQuery | DrawSortKey                                     │
├──────────────────────────────────────────────────────────────────────┤
│  Advanced Rendering Systems                                          │
│  DDGIProbeSystem | AdaptiveProbeVolumes | HybridRTManager            │
│  GPUDrivenRenderer | GPUOcclusionCulling | MeshShaderPipeline         │
│  MeshClusterSystem | VirtualTexture | ShaderGraphCompiler             │
├──────────────────────────────────────────────────────────────────────┤
│                     RHI Abstraction Layer                            │
│  IRHIDevice | IRHICommandList | IRHISwapChain                        │
│  D3D11 | D3D12 | Vulkan | OpenGL | Metal (WIP) | NullRHI            │
└──────────────────────────────────────────────────────────────────────┘
         │          │           │            │           │          │
         ▼          ▼           ▼            ▼           ▼          ▼
       DX11 API  DX12 API  Vulkan API  OpenGL API  Metal API   No-op
```

### Key Source Files

| File | Responsibility |
|------|---------------|
| `GraphicsEngine.h` | Central engine class -- device creation, frame management, render dispatch |
| `MaterialSystem.h` | PBR material management, texture slots, variants, hot-reload |
| `TextureSystem.h` | Texture loading, streaming, LRU eviction, quality settings |
| `LightManager.h` | Per-frame light culling, tile binning, shadow atlas |
| `LightingSystem.h` | Deferred lighting pass, light components, environment lighting |
| `PostProcessing.h` | HDR pipeline: bloom, tone mapping, color grading |
| `PostProcessingPipeline.h` | Composable post-processing effect chain |
| `RHI/RHIDevice.h` | Abstract device interface for all backends |
| `RHI/RHIFactory.h` | Backend factory (creates D3D11, Vulkan, or OpenGL device) |
| `FrustumCulling.h` | Camera frustum-based visibility culling |
| `MeshLOD.h` | Automatic LOD generation with distance-based switching |
| `DecalSystem.h` | Projected decals (bullet holes, blood, scorch marks) |
| `ParticleSystem.h` | GPU-accelerated particle system |
| `DDGIProbeSystem.h` | Dynamic Diffuse GI with probe grids and spherical harmonics |
| `AdaptiveProbeVolumes.h` | Brick-based hierarchical GI probes with 3 LOD levels |
| `HybridRT/HybridRTManager.h` | Hybrid ray tracing pipeline with SDF fallback |
| `VirtualTexture.h` | Feedback-driven virtual texture streaming with LRU cache |
| `MeshShaderPipeline.h` | Meshlet pipeline with amplification/mesh shaders |
| `GPUDrivenRenderer.h` | Compute-based frustum culling and indirect draw generation |
| `GPUOcclusionCulling.h` | Hierarchical Z-buffer occlusion testing |
| `MeshClusterSystem.h` | DAG-based cluster hierarchy for virtual geometry |
| `LODGenerator.h` | Automatic mesh simplification via edge collapse |
| `ShaderGraph/ShaderGraphCompiler.h` | Node-based shader graph to HLSL compilation |
| `RHI/DXRSupport.h` | DXR 1.1 ray tracing (reflections, shadows, AO, GI) |
| `RHI/D3D12Device.h` | DirectX 12 backend with mesh shaders, DXR, VRS |
| `RenderGraph/RenderGraph.h` | Declarative render pass graph |

---

## Render Pipelines

Four render paths are available via the `RenderPath` enum:

```cpp
enum class RenderPath {
    Forward,     // Traditional forward rendering
    Deferred,    // G-buffer based deferred rendering
    ForwardPlus, // Tiled forward rendering (Forward+)
    Clustered    // Clustered rendering with 3D light culling
};
```

| Pipeline | Max Lights | Transparency | Best For |
|----------|-----------|--------------|----------|
| `Forward` | ~16 per object | Native alpha blend | Simple scenes, mobile |
| `Deferred` | Hundreds | Requires separate pass | Many lights, indoor |
| `ForwardPlus` | Thousands (tiled) | Native alpha blend | Open worlds, mixed |
| `Clustered` | Thousands (3D grid) | Native alpha blend | Complex scenes |

### Deferred Rendering G-Buffer Layout

The deferred pipeline writes to a 4-target G-buffer:

| G-Buffer | Format | Contents |
|----------|--------|----------|
| GBuffer[0] | R8G8B8A8 | Albedo (RGB) + Material ID (A) |
| GBuffer[1] | R16G16B16A16 | World Normal (RGB) + Roughness (A) |
| GBuffer[2] | R8G8B8A8 | Metallic (R) + Occlusion (G) + Emissive (B) + Flags (A) |
| GBuffer[3] | R16G16 | Motion Vectors (RG) for TAA/motion blur |

### Forward+ Tile Binning

Forward+ divides the screen into tiles and assigns lights per tile:

```
Screen (1920x1080) divided into 16x16 pixel tiles
  = 120 x 68 = 8,160 tiles

Each tile stores up to 256 light indices (TileLightList)

Per frame:
  1. Frustum-cull all lights
  2. Project visible lights to screen space
  3. Assign lights to overlapping tiles
  4. Each pixel reads its tile's light list
```

---

## Quality Presets

```cpp
enum class QualityPreset {
    Low,    // Mobile/integrated graphics -- low shadow res, no MSAA, minimal post-process
    Medium, // Mid-range hardware -- 1024 shadows, MSAA 2x, bloom
    High,   // High-end hardware -- 2048 shadows, MSAA 4x, SSAO + bloom
    Ultra,  // Enthusiast hardware -- 4096 shadows, MSAA 8x, all effects
    Custom  // User-defined settings
};
```

### GraphicsSettings Structure

```cpp
struct GraphicsSettings {
    // Rendering
    RenderPath renderPath       = RenderPath::Deferred;
    QualityPreset qualityPreset = QualityPreset::High;
    bool vsync                  = true;
    bool hdr                    = true;
    uint32_t msaaSamples        = 4;

    // Textures
    uint32_t maxTextureSize     = 2048;
    bool anisotropicFiltering   = true;
    uint32_t anisotropyLevel    = 16;

    // Shadows
    bool shadows                = true;
    uint32_t shadowMapSize      = 2048;
    uint32_t cascadeCount       = 3;

    // Post-processing
    bool bloom      = true;
    bool ssao       = false;
    bool taa        = false;
    bool motionBlur = false;

    // Performance
    bool frustumCulling    = true;
    bool occlusionCulling  = false;
    bool levelOfDetail     = true;
    uint32_t maxDrawCalls  = 1000;

    // Debug
    bool wireframeMode     = false;
    bool debugMode         = false;
    bool showFPS           = false;
    float clearColor[4]    = {0, 0, 0, 1};
    float renderScale      = 1.0f;
    bool enableGPUTiming   = false;
};
```

---

## PBR Material System

The `MaterialSystem` implements a physically-based metallic/roughness workflow with 18 texture slots.

### PBR Properties

```cpp
struct PBRProperties {
    XMFLOAT4 albedoColor       = {1, 1, 1, 1};  // Base color (RGBA)
    float metallicFactor       = 0.0f;            // 0 = dielectric, 1 = metallic
    float roughnessFactor      = 0.5f;            // 0 = mirror, 1 = fully rough
    float normalScale          = 1.0f;            // Normal map intensity
    float occlusionStrength    = 1.0f;            // AO strength
    XMFLOAT3 emissiveColor     = {0, 0, 0};      // Emissive RGB
    float emissiveFactor       = 0.0f;            // Emissive intensity
    float alphaCutoff          = 0.5f;            // Alpha test threshold
    float indexOfRefraction    = 1.5f;            // IOR for dielectrics
};
```

### Texture Slots

```cpp
enum class MaterialTextureType {
    Albedo,             // Base color/albedo
    Normal,             // Normal map
    Metallic,           // Metalness map
    Roughness,          // Roughness map
    Occlusion,          // Ambient occlusion
    Emissive,           // Self-illumination
    Height,             // Parallax/displacement
    DetailAlbedo,       // Detail albedo overlay
    DetailNormal,       // Detail normal overlay
    Subsurface,         // Subsurface scattering
    Transmission,       // Light transmission
    Clearcoat,          // Clear coat layer
    ClearcoatRoughness, // Clear coat roughness
    Anisotropy,         // Anisotropic direction
    Custom0,            // Custom slot 0
    Custom1,            // Custom slot 1
    Custom2,            // Custom slot 2
    Custom3             // Custom slot 3
};
```

### Advanced Material Properties

```cpp
struct AdvancedProperties {
    // Subsurface scattering
    bool subsurfaceEnabled;  XMFLOAT3 subsurfaceColor;  float subsurfaceRadius;
    // Clearcoat
    bool clearcoatEnabled;   float clearcoatFactor;  float clearcoatRoughness;
    // Anisotropy
    bool anisotropyEnabled;  float anisotropyFactor;  XMFLOAT2 anisotropyDirection;
    // Transmission
    bool transmissionEnabled;  float transmissionFactor;  XMFLOAT3 transmissionColor;
    // Sheen (fabric)
    bool sheenEnabled;  XMFLOAT3 sheenColor;  float sheenRoughness;
    // Iridescence
    bool iridescenceEnabled;  float iridescenceFactor;  float iridescenceIOR;
    float iridescenceThickness;
};
```

### Blend Modes

```cpp
enum class BlendMode {
    Opaque,      // Fully opaque (default)
    AlphaTest,   // Alpha testing / cutout
    Transparent, // Alpha blending
    Additive,    // Additive blending (fire, sparks)
    Multiply,    // Multiplicative blending
    Screen       // Screen blending
};
```

### Material Render State

```cpp
struct MaterialRenderState {
    BlendMode blendMode    = BlendMode::Opaque;
    CullMode cullMode      = CullMode::Back;
    bool depthTest         = true;
    bool depthWrite        = true;
    bool castShadows       = true;
    bool receiveShadows    = true;
    int renderQueue        = 2000;    // Sort priority
    bool doubleSided       = false;
};
```

---

## Anti-Aliasing

| Method | Enum | Quality | Performance | Notes |
|--------|------|---------|-------------|-------|
| None | `MSAALevel::None` | -- | Best | No anti-aliasing |
| MSAA 2x | `MSAALevel::MSAA2x` | Low | Good | 2x multi-sample |
| MSAA 4x | `MSAALevel::MSAA4x` | Medium | Moderate | 4x multi-sample (default High preset) |
| MSAA 8x | `MSAALevel::MSAA8x` | High | Expensive | 8x multi-sample |
| FXAA | `PostProcessEffect::FXAA` | Low | Cheap | Post-process, slight blur |
| TAA | `TAASettings` | High | Moderate | Temporal, reduces shimmer, requires motion vectors |

### TAA Settings

TAA uses jittered projection matrices across frames to accumulate sub-pixel samples. Requires motion vector G-buffer output.

---

## Post-Processing Pipeline

The post-processing system provides a composable chain of effects.

### Post-Process Effect Types

```cpp
enum class PostProcessEffect {
    None, Bloom, ToneMapping, ColorGrading, FXAA, TAA, SSAO, SSR,
    MotionBlur, DepthOfField, Vignette, ChromaticAberration,
    FilmGrain, LensDistortion, LightShafts, LensFlare
};
```

### Bloom Settings

```cpp
struct BloomSettings {
    bool enabled    = true;
    float threshold = 1.0f;     // Brightness threshold for bloom extraction
    float intensity = 1.0f;     // Bloom strength multiplier
    float radius    = 1.0f;     // Blur radius
    float softKnee  = 0.5f;    // Smooth transition at threshold
    int iterations  = 6;        // Number of downsample/upsample passes
    XMFLOAT3 tint   = {1,1,1}; // Bloom color tint
};
```

### Tone Mapping

```cpp
enum class ToneMappingOperator {
    None,           // Linear (no tone mapping)
    Reinhard,       // Simple Reinhard
    ReinhardJodie,  // Reinhard-Jodie variant
    Uncharted2,     // Uncharted 2 filmic curve
    ACES,           // Academy Color Encoding System (default)
    AgX,            // AgX tone mapping
    FilmicALU,      // Filmic ALU approximation
    Custom          // Custom LUT-based curve
};

struct ToneMappingSettings {
    ToneMappingOperator operator_ = ToneMappingOperator::ACES;
    float exposure    = 1.0f;        // Exposure multiplier
    float gamma       = 2.2f;        // Display gamma
    float whitePoint  = 11.2f;       // White point luminance
    XMFLOAT3 colorBalance = {1,1,1}; // RGB color balance
};
```

### Color Grading

```cpp
struct ColorGradingSettings {
    bool enabled     = false;
    float temperature = 0.0f;    // Color temperature shift
    float tint        = 0.0f;    // Green-magenta tint
    float contrast    = 1.0f;    // Contrast adjustment
    float brightness  = 0.0f;    // Brightness offset
    float saturation  = 1.0f;    // Color saturation
    XMFLOAT3 lift     = {1,1,1}; // Shadow color adjustment
    XMFLOAT3 gamma    = {1,1,1}; // Midtone color adjustment
    XMFLOAT3 gain     = {1,1,1}; // Highlight color adjustment
};
```

### SSAO Settings

```cpp
struct SSAOSettings {
    bool enabled     = false;
    float radius     = 0.5f;    // Sampling radius in world units
    float intensity  = 1.0f;    // SSAO darkening intensity
    int sampleCount  = 16;      // Hemisphere samples (16, 32, or 64)
    float bias       = 0.025f;  // Depth bias to prevent self-occlusion
    bool blur        = true;    // Bilateral blur pass for noise reduction
};
```

### SSR Settings

```cpp
struct SSRSettings {
    bool enabled     = false;
    float maxDistance = 100.0f;  // Maximum ray march distance
    int maxSteps     = 32;      // Maximum ray march iterations
    float thickness  = 0.5f;    // Surface thickness for hit detection
    float fadeStart  = 80.0f;   // Distance to begin fading reflections
    float fadeEnd    = 100.0f;  // Distance to fully fade reflections
};
```

### Volumetric Lighting Settings

```cpp
struct VolumetricSettings {
    bool enabled      = false;
    int sampleCount   = 32;     // Ray march samples
    float scattering  = 0.1f;   // Light scattering coefficient
    float extinction  = 0.01f;  // Light extinction coefficient
    float anisotropy  = 0.3f;   // Henyey-Greenstein phase function parameter
};
```

---

## Shadow Mapping

| Method | Description | Quality | Performance |
|--------|-------------|---------|-------------|
| PCF | Percentage-closer filtering | Medium | Fast |
| VSM | Variance shadow maps | Smooth | Medium |
| CSM | Cascaded shadow maps (up to 3 cascades) | High | Moderate |
| PCSS | Percentage-closer soft shadows | Very high | Expensive |

The `LightManager` manages a shadow atlas with up to 16 slots:

```cpp
struct ShadowAtlasSlot {
    uint32_t lightId;       // Owning light ID
    int size;               // Shadow map resolution
    bool inUse;             // Whether slot is allocated
    float lastUpdateTime;   // Last update timestamp
};
```

---

## Light Management

### RuntimeLight

```cpp
struct RuntimeLight {
    uint32_t id;
    RuntimeLightType type;      // Directional, Point, Spot
    XMFLOAT3 position;
    XMFLOAT3 direction;
    XMFLOAT3 color;
    float intensity;
    float range;
    float innerAngle, outerAngle;  // Spot light cone
    bool castsShadows;
    int shadowMapSize;
    float shadowBias;
    bool enabled, isVisible;
};
```

### LightManager Per-Frame Flow

```
BeginFrame(viewProjMatrix)
    │
    ▼
SubmitLight() x N          ◄── Called by LightingSystem for each active light
    │
    ▼
CullAndBinLights()
    ├── Frustum cull each light (sphere test for point/spot)
    ├── Directional lights: assigned to ALL tiles
    ├── Point/Spot lights: projected to screen, assigned to overlapping tiles
    └── Update metrics (visible count, max lights per tile)
    │
    ▼
GetVisibleLights()         ◄── Consumed by render passes
GetShadowCasters()         ◄── Consumed by shadow pass
GetTileLightList(x, y)     ◄── Consumed by Forward+ pixel shader
```

### Tile Light List

```cpp
struct TileLightList {
    static constexpr int MAX_LIGHTS_PER_TILE = 256;
    uint32_t lightIndices[MAX_LIGHTS_PER_TILE];
    int lightCount;
};
```

---

## RHI Abstraction Layer

The Render Hardware Interface provides backend-agnostic graphics through abstract interfaces.

### Core Interfaces

| Interface | Description |
|-----------|-------------|
| `IRHIDevice` | Resource creation, command submission, frame management |
| `IRHICommandList` | GPU command recording (draw, dispatch, state changes) |
| `IRHISwapChain` | Present to screen, back buffer management |
| `IRHIBuffer` | Vertex, index, and constant buffers |
| `IRHITexture` | 2D textures, cubemaps, render targets |
| `IRHIShader` | Compiled shader programs |
| `IRHISampler` | Texture sampling states |
| `IRHIPipelineState` | Combined render state objects |

### Backend Implementations

| Backend | File | Status | Platform |
|---------|------|--------|----------|
| DirectX 11 | `RHI/D3D11/D3D11Device.h` | In-profile implementation; blocked and uncertified | Windows 11 x64 stable-v1 target |
| DirectX 12 | `RHI/D3D12/D3D12Device.h` | Experimental | Windows |
| Vulkan | `RHI/Vulkan/VulkanDevice.h` | Experimental, incomplete renderer parity | Desktop development paths |
| OpenGL | `RHI/OpenGL/OpenGLDevice.h` | Experimental, context/host dependent | Desktop development paths |
| Metal | `RHI/Metal/MetalDevice.h` plus Objective-C++ sources | Partial implementation | macOS development path |
| DXR | `RHI/DXRSupport.h` | Experimental ray-tracing source path | Eligible Windows D3D12 builds |

### IRHIDevice Key Methods

```cpp
class IRHIDevice {
    virtual bool Initialize(const RHIDeviceDesc& desc) = 0;
    virtual void Shutdown() = 0;

    // Resource creation
    virtual IRHIBuffer* CreateBuffer(const RHIBufferDesc& desc) = 0;
    virtual IRHITexture* CreateTexture(const RHITextureDesc& desc) = 0;
    virtual IRHIShader* CreateShader(const RHIShaderDesc& desc) = 0;
    virtual IRHIPipelineState* CreatePipelineState(...) = 0;

    // Command submission
    virtual IRHICommandList* GetImmediateCommandList() = 0;
    virtual IRHICommandList* CreateDeferredCommandList() = 0;
    virtual void ExecuteCommandList(IRHICommandList* commandList) = 0;

    // Frame management
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void WaitForIdle() = 0;
};
```

---

## Texture System

`TextureSystem` manages texture loading, streaming, and memory via LRU eviction.

### Texture Formats

```cpp
enum class TextureFormat {
    R8G8B8A8_UNORM, R8G8B8A8_SRGB,       // Standard 8-bit
    BC1_UNORM, BC1_SRGB,                   // DXT1 (4:1 compression)
    BC3_UNORM, BC3_SRGB,                   // DXT5 (4:1 with alpha)
    BC7_UNORM, BC7_SRGB,                   // High-quality compression
    R16G16B16A16_FLOAT, R32G32B32A32_FLOAT, // HDR formats
    D24_UNORM_S8_UINT,                     // Depth-stencil
    R16_FLOAT, R32_FLOAT                    // Single-channel float
};
```

### Quality Levels

```cpp
enum class TextureQuality {
    Low,    // Quarter resolution, high compression
    Medium, // Half resolution, medium compression
    High,   // Full resolution, low compression
    Ultra   // Full resolution, no compression
};
```

### LRU Eviction

The texture system tracks per-texture usage data for smart eviction:

```cpp
struct TextureLRUData {
    uint64_t lastUsedFrame;         // Frame number when last bound
    float screenCoverage;           // Screen-space coverage estimate
    float distanceToCamera;         // Distance to active camera
    uint8_t priority;               // 0=background .. 5=pinned
    bool pinned;                    // Never evict if true
};
```

Eviction score = `priority * 1000 - frameSinceLastUse + coverage * 5000`. Lower scores are evicted first. Default memory budget is 512 MB.

---

## Renderer Integration Systems

| System | File | Purpose |
|--------|------|---------|
| `PipelineStateCache` | `PipelineStateCache.h` | Hash-based D3D11 state deduplication |
| `RenderTargetPool` | `RenderTargetPool.h` | Pooled transient render target recycling |
| `GPUSceneBuffer` | `GPUSceneBuffer.h` | Persistent GPU buffer for instance data |
| `BVHAccelerator` | `BVHAccelerator.h` | SAH-based BVH for hierarchical frustum culling |
| `ConstantBufferRing` | `ConstantBufferRing.h` | Ring-buffer sub-allocation for constant buffers |
| `GPUDebugMarkers` | `GPUDebugMarkers.h` | PIX/RenderDoc GPU event annotations |
| `GPUTimestampQuery` | `GPUTimestampQuery.h` | Per-pass GPU timing queries |
| `DrawSortKey` | `DrawSortKey.h` | 64-bit sort keys for draw call ordering |

---

## Render Statistics

```cpp
struct RenderStatistics {
    // Performance
    float frameTime, cpuTime, gpuTime;  uint32_t fps;
    // Rendering
    uint32_t drawCalls, triangles, vertices, textureBinds, materialSwitches;
    // Culling
    uint32_t totalObjects, visibleObjects, culledObjects;  float cullingTime;
    // Memory
    size_t textureMemory, meshMemory, totalGPUMemory;
    // Lighting
    uint32_t activeLights, shadowUpdates;  float lightCullingTime;
    // Post-processing
    float postProcessTime;  uint32_t postProcessPasses;
};
```

---

## ECS Draw Submission

The ECS `RenderSystem` submits meshes to the graphics engine each frame:

```cpp
struct MeshDrawCommand {
    std::string meshPath;
    std::string materialPath;
    DirectX::XMFLOAT4X4 worldMatrix;
    bool castShadows = true;
};

// Per entity:
graphics.SubmitMeshForRendering(meshPath, materialPath, worldMatrix, castShadows);

// During render:
graphics.ProcessDrawList(viewMatrix, projMatrix);
```

---

## Thread Safety

- `GraphicsEngine` -- Main thread for all render operations. Uses `std::atomic<bool> m_frameInProgress` for frame state. Metrics access protected by `std::mutex m_metricsMutex`.
- `TextureSystem` -- Main thread for render operations. Background worker threads for async texture loading and streaming. Cache access protected by `std::mutex m_texturesMutex`.
- `MaterialSystem` -- Metrics access protected by `std::mutex m_metricsMutex`. Hot-reload runs on main thread only.
- `LightManager` -- Single-threaded (call from main render thread only).

---

## Console Commands

The graphics engine registers 200+ debug commands. Common ones:

```
graphics_info              # GPU adapter, driver version, VRAM, feature level
render_path <path>         # Switch: forward, deferred, forward+, clustered
quality <preset>           # Set quality: low, medium, high, ultra
wireframe                  # Toggle wireframe rendering
ssao <on|off>              # Toggle screen-space ambient occlusion
ssr <on|off>               # Toggle screen-space reflections
bloom <on|off>             # Toggle bloom
msaa <1|2|4|8>             # Set MSAA sample count
vsync <on|off>             # Toggle vertical sync
screenshot <filename>      # Save screenshot to file
shader_reload              # Hot-reload all shaders
```

---

## Headless Mode and Software Rendering

These are distinct development routes, not interchangeable automatic
fallbacks or release certification.

### NullRHIDevice (Headless -- No Rendering)

`RHIFactory::CreateDevice()` selects and constructs one requested device; it
does not retry the whole backend sequence. `RHIBridge::Initialize()` can retry
candidates after device or swap-chain failures and finally construct
`NullRHIDevice`. However, the current shared/server headless entry points pass
null graphics state and do not instantiate NullRHI. Closing and certifying that
integration is tracked by HEAD-220, so this page does not claim that every
non-rendering subsystem or host lifecycle is functional headlessly.

### Software Rendering via OpenGL + Mesa llvmpipe

For environments that need **real pixel output** without a GPU, the OpenGL backend renders entirely on CPU using Mesa's llvmpipe software rasterizer:

```bash
# 1. Start a virtual X11 display (one-time, or use an existing display)
Xvfb :99 -screen 0 1920x1080x24 &

# 2. Run the engine with software rendering
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./SparkEngine
```

**Development-route boundary:** when the explicitly selected Mesa/X11/EGL
stack initializes, llvmpipe can provide real OpenGL pixel output rather than
`NullRHI` no-ops. `NullRHI` does not perform software rendering. Exact context
extensions, shader support, CPU requirements, and renderer-pass behavior come
from the installed Mesa/runtime configuration and are not release-certified.

**Source:** `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp` (GLX bootstrap, EGL headless path)
**Dependencies:** GLAD (bundled in `ThirdParty/glad/`), Mesa (system), X11 (system)

---

## Error Handling

- `GraphicsEngine::Initialize()` returns `HRESULT`. Check with `FAILED()` macro.
- Device creation validates feature level support (D3D_FEATURE_LEVEL_11_0 minimum).
- `RHIFactory::CreateDevice()` constructs one selected device. Retry and
  NullRHI fallback live in `RHIBridge::Initialize()`; whole-engine headless
  continuation remains unproven until HEAD-220 closes.
- Render target creation validates format support and MSAA sample count.
- Material loading returns `nullptr` for missing files (falls back to error material with magenta color).
- Texture streaming failures are logged and fall back to default white/black textures.
- `Material::ValidatePBRProperties()` uses `ASSERT_MSG` to validate ranges at development time.

---

## Performance Considerations

- **Frustum culling**: BVH-accelerated, reduces draw calls by culling off-screen objects.
- **Draw call sorting**: 64-bit sort keys minimize state changes (material, shader, depth).
- **Pipeline state cache**: Deduplicates D3D11 blend/depth/rasterizer state objects.
- **Constant buffer ring**: Avoids per-frame CB creation via ring-buffer sub-allocation.
- **Render target pool**: Recycles transient render targets to reduce allocation churn.
- **Texture streaming**: Background threads load textures asynchronously.
- **LRU eviction**: Automatically evicts unused textures when VRAM budget is exceeded.
- **GPU timestamp queries**: Per-pass timing for identifying bottlenecks.

---

## Troubleshooting

### Black screen on startup
- Check `HRESULT` from `GraphicsEngine::Initialize()`
- Verify GPU supports D3D_FEATURE_LEVEL_11_0
- Check swap chain creation (window handle must be valid)

### Low FPS / high frame time
- Run `profile_gpu` to identify the bottleneck pass
- Check `graphics_info` for VRAM pressure
- Try `quality low` to rule out shader complexity
- Disable expensive effects: `ssao off; ssr off; bloom off`

### Materials appear magenta
- The error material indicates a failed material load
- Check file paths in material definitions
- Run `material_list` to verify loaded materials

### Texture pop-in
- Increase streaming thread count via `TextureSystem::SetStreamingThreadCount()`
- Increase memory budget via `Console_SetMemoryBudget()`
- Pin critical textures via `TextureSystem::PinTexture()`

---

## See Also

- [Global Illumination](../graphics/Global-Illumination.md) -- DDGI, Adaptive Probe Volumes, hybrid ray tracing
- [GPU Driven Rendering](../graphics/GPU-Driven-Rendering.md) -- Compute culling, HiZ occlusion, indirect draws
- [Mesh Shaders](../graphics/Mesh-Shaders.md) -- Meshlet pipeline, amplification/mesh shaders
- [Virtual Texturing](../graphics/Virtual-Texturing.md) -- Feedback-driven page streaming
- [DXR Raytracing](../graphics/DXR-Raytracing.md) -- Ray-traced reflections, shadows, AO, GI
- [Hybrid Ray Tracing](../graphics/Hybrid-Ray-Tracing.md) -- Software SDF + hardware DXR pipeline
- [Shader Graph](../graphics/Shader-Graph.md) -- Node-based visual material authoring
- [Render Graph](../graphics/Render-Graph.md) -- Declarative render pass system
- [RHI Abstraction Layer](../graphics/RHI-Abstraction-Layer.md) -- Multi-backend device interface
- [D3D12 Backend](../graphics/D3D12-Backend.md) -- DirectX 12 features
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- Shader authoring and compilation
- [Entity Component System](Entity-Component-System.md) -- MeshRenderer, LightComponent, and ParticleEmitter components
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- Model and texture loading
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Material editor and visual tools
- [Animation](Animation.md) -- Skeletal animation and blending
- [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md) -- Procedural mesh and terrain rendering
- [Physics](Physics.md) -- Debug draw overlay for collision shapes
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md) -- Dynamic lighting and weather effects
