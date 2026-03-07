# Rendering and Graphics

SparkEngine's rendering system is built on DirectX 11 with experimental Vulkan and OpenGL backends through a Render Hardware Interface (RHI) abstraction layer.

**Source:** `SparkEngine/Source/Graphics/`

## Render Pipelines

Four render paths are available via the `RenderPath` enum:

| Pipeline | Description |
|----------|-------------|
| `Forward` | Traditional forward rendering — simple, good for transparent objects |
| `Deferred` | G-buffer based deferred rendering — efficient with many lights |
| `ForwardPlus` | Tiled forward rendering — combines benefits of forward and deferred |
| `Clustered` | Clustered rendering — 3D light culling for complex scenes |

## Quality Presets

```cpp
enum class QualityPreset { Low, Medium, High, Ultra, Custom };
```

Each preset configures shadow resolution, MSAA level, texture quality, post-processing effects, and draw distances.

## PBR Material System

The `MaterialSystem` implements a physically-based metallic/roughness workflow with 18+ texture slots:

| Texture Slot | Description |
|-------------|-------------|
| Albedo | Base color |
| Normal | Normal mapping |
| Metallic | Metalness (0 = dielectric, 1 = metal) |
| Roughness | Surface roughness |
| Occlusion | Ambient occlusion |
| Emissive | Self-illumination |
| Height | Parallax/displacement mapping |
| Subsurface | Subsurface scattering |
| Clearcoat | Clear coat layer |
| Anisotropy | Anisotropic reflections |
| Transmission | Light transmission |
| Sheen | Fabric-like sheen |

**Blend modes:** Opaque, AlphaTest, Transparent, Additive, Multiply, Screen

## Anti-Aliasing

| Method | Enum | Description |
|--------|------|-------------|
| None | `MSAALevel::None` | No anti-aliasing |
| MSAA 2x | `MSAALevel::MSAA2x` | 2x multi-sample |
| MSAA 4x | `MSAALevel::MSAA4x` | 4x multi-sample |
| MSAA 8x | `MSAALevel::MSAA8x` | 8x multi-sample |
| FXAA | — | Fast approximate anti-aliasing (post-process) |
| TAA | `TAASettings` | Temporal anti-aliasing with jittered projection |

## Post-Processing Pipeline

The `PostProcessingPipeline` provides a composable chain of effects:

### Screen-Space Effects
- **SSAO** — Screen-space ambient occlusion (configurable radius, intensity, sample count, bias)
- **SSR** — Screen-space reflections (ray marching with configurable max distance and steps)

### Bloom and Tone Mapping
- **Bloom** — Multi-pass bloom with soft knee threshold and configurable iterations
- **HDR Tone Mapping** — Reinhard, ACES, Uncharted 2, AgX, FilmicALU operators

### Color Grading
- Temperature and tint adjustment
- Saturation control
- Lift/gamma/gain curves

### Atmospheric Effects
- **Volumetric Lighting** — Light shafts with configurable density and samples
- **Fog** — Distance and height-based fog system (`FogSystem`)
- **Weather** — Weather rendering effects (`WeatherSystem`)

### Camera Effects
- Motion blur
- Depth of field
- Vignette
- Chromatic aberration
- Film grain
- Lens distortion
- Lens flare

## SSAO Settings

```cpp
struct SSAOSettings {
    bool  enabled     = false;
    float radius      = 0.5f;
    float intensity   = 1.0f;
    int   sampleCount = 16;
    float bias        = 0.025f;
    bool  blur        = true;
};
```

## SSR Settings

```cpp
struct SSRSettings {
    bool  enabled     = false;
    float maxDistance  = 100.0f;
    int   maxSteps    = 32;
    float thickness   = 0.5f;
    float fadeStart   = 80.0f;
    float fadeEnd     = 100.0f;
};
```

## Shadow Mapping

- **PCF** — Percentage-closer filtering
- **VSM** — Variance shadow maps
- **CSM** — Cascaded shadow maps (up to 3 cascades)
- **PCSS** — Percentage-closer soft shadows

## RHI Abstraction Layer

The Render Hardware Interface provides backend-agnostic graphics:

| File | Description |
|------|-------------|
| `RHI.h` | Master include |
| `RHIFactory.h` | Backend factory (creates D3D11, Vulkan, or OpenGL device) |
| `RHIDevice.h` | Abstract device interface |
| `RHITypes.h` | Unified type definitions |
| `RHIResources.h` | Abstract resource interfaces |
| `RHIBridge.h` | High-level integration bridge |

**Backend implementations:**
- `D3D11Device.h` — DirectX 11 (primary, fully featured)
- `VulkanDevice.h` — Vulkan (experimental)
- `OpenGLDevice.h` — OpenGL (experimental)
- `DXRSupport.h` — DirectX Raytracing (optional, requires D3D12)

## Texture System

`TextureSystem` manages texture loading and streaming:

- Multiple formats (R8G8B8A8, BC compression, HDR, depth)
- Async streaming with background loading
- Quality levels (Low/Medium/High/Ultra)
- Dynamic texture creation
- Hot-reloading during development

## Mesh and LOD

- `MeshLOD` — Automatic LOD generation with distance-based switching
- `FrustumCulling` — Camera frustum-based visibility culling
- `DecalSystem` — Projected decals (bullet holes, blood, scorch marks)

## GPU Particle System

`ParticleSystem` provides GPU-accelerated particles controlled via `ParticleEmitterComponent`.

## Lighting

- `LightManager` — Manages all light sources
- `LightingSystem` — Deferred lighting pass, light culling, shadow updates
- **IBL** — Image-Based Lighting with environment maps

## Console Commands

The graphics engine registers 200+ debug commands. Common ones:

```
graphics_info          # GPU and adapter information
render_path <path>     # Switch render pipeline (forward/deferred/forward+/clustered)
quality <preset>       # Set quality preset (low/medium/high/ultra)
wireframe              # Toggle wireframe rendering
ssao <on|off>          # Toggle SSAO
ssr <on|off>           # Toggle SSR
bloom <on|off>         # Toggle bloom
msaa <1|2|4|8>         # Set MSAA level
vsync <on|off>         # Toggle vertical sync
```

## See Also

- [[Shader Pipeline]] — Shader authoring and compilation
- [[Asset Pipeline]] — Model and texture loading
- [[SparkEditor]] — Material editor and visual tools
