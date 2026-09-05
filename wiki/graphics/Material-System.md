# Material System

SparkEngine uses a physically-based rendering (PBR) material system built on the metallic/roughness workflow. Materials are GPU-compiled objects that encapsulate surface properties, texture bindings, blend/cull state, and shader permutations. The system supports 18 texture slots, advanced shading models (subsurface scattering, clearcoat, anisotropy, transmission, sheen, iridescence), material variants for shader permutations, hot-reload, and thread-safe caching.

**Source:** `SparkEngine/Source/Graphics/MaterialSystem.h`

## Overview

```
MaterialSystem (singleton manager)
    ├── Material cache           (thread-safe, keyed by name)
    ├── Texture cache            (shared SRVs, deduplicated by path)
    ├── Sampler cache            (hashed TextureSampling → ID3D11SamplerState)
    ├── Default material         (white dielectric, roughness 0.5)
    └── Error material           (magenta fallback for missing materials)

Material (individual asset)
    ├── PBRProperties            (albedo, metallic, roughness, normal, AO, emissive, IOR)
    ├── AdvancedProperties       (SSS, clearcoat, anisotropy, transmission, sheen, iridescence)
    ├── MaterialRenderState      (blend mode, cull mode, depth, shadows, render queue)
    ├── MaterialTexture[18]      (SRV + sampling + tiling/offset per slot)
    ├── Variants                 (named shader define sets)
    └── Compiled pipeline state  (BlendState, DepthStencilState, RasterizerState, CB)
```

Materials are compiled once via `CompileMaterial()`, which creates the D3D11 blend, depth-stencil, and rasterizer states along with a GPU-aligned constant buffer (`MaterialConstants`). Binding a material sets all texture SRVs, samplers, the constant buffer, and pipeline states on the device context.

## PBR Workflow

The material system implements the metallic/roughness PBR model. Core surface parameters are stored in `PBRProperties` and uploaded to the GPU via a 16-byte-aligned constant buffer:

```cpp
struct alignas(16) MaterialConstants
{
    XMFLOAT4 albedoColor;    // Base color (RGBA)
    float metallicFactor;    // 0 = dielectric, 1 = metallic
    float roughnessFactor;   // 0 = mirror, 1 = fully rough
    float normalScale;       // Normal map intensity
    float occlusionStrength; // Ambient occlusion strength
    XMFLOAT3 emissiveColor;  // Emissive RGB
    float emissiveFactor;    // Emissive intensity multiplier
    float alphaCutoff;       // Alpha test threshold
    float indexOfRefraction; // IOR for dielectrics (default 1.5)
    float pad0, pad1;        // Padding to 16-byte alignment
};
```

| Property | Range | Default | Description |
|----------|-------|---------|-------------|
| `albedoColor` | RGBA [0,1] | (1, 1, 1, 1) | Base surface color |
| `metallicFactor` | [0, 1] | 0.0 | Metal vs. dielectric response |
| `roughnessFactor` | [0, 1] | 0.5 | Microsurface roughness |
| `normalScale` | any | 1.0 | Normal map intensity multiplier |
| `occlusionStrength` | [0, 1] | 1.0 | Ambient occlusion strength |
| `emissiveColor` | RGB [0,+inf) | (0, 0, 0) | Self-illumination color |
| `emissiveFactor` | [0, +inf) | 0.0 | Emissive intensity |
| `alphaCutoff` | [0, 1] | 0.5 | Alpha test discard threshold |
| `indexOfRefraction` | (0, +inf) | 1.5 | Fresnel IOR for dielectrics |

All scalar PBR properties are validated at set-time via `Material::ValidatePBRProperties()`.

## Texture Slots

Each material supports up to 18 texture slots. Every slot carries its own SRV, sampling parameters, UV tiling/offset, intensity, and enable flag.

| Slot | Enum | Description |
|------|------|-------------|
| 0 | `Albedo` | Base color / albedo map |
| 1 | `Normal` | Tangent-space normal map |
| 2 | `Metallic` | Metallic map (grayscale) |
| 3 | `Roughness` | Roughness map (grayscale) |
| 4 | `Occlusion` | Ambient occlusion map |
| 5 | `Emissive` | Emissive map |
| 6 | `Height` | Height / displacement map |
| 7 | `DetailAlbedo` | Detail albedo (tiled overlay) |
| 8 | `DetailNormal` | Detail normal map |
| 9 | `Subsurface` | Subsurface scattering color map |
| 10 | `Transmission` | Transmission map |
| 11 | `Clearcoat` | Clearcoat layer intensity map |
| 12 | `ClearcoatRoughness` | Clearcoat roughness map |
| 13 | `Anisotropy` | Anisotropy direction map |
| 14 | `Custom0` | Custom texture slot 0 |
| 15 | `Custom1` | Custom texture slot 1 |
| 16 | `Custom2` | Custom texture slot 2 |
| 17 | `Custom3` | Custom texture slot 3 |

### Texture Sampling

Each slot has independent sampling parameters:

```cpp
struct TextureSampling
{
    D3D11_FILTER filter = D3D11_FILTER_ANISOTROPIC;  // Default: anisotropic
    D3D11_TEXTURE_ADDRESS_MODE addressU = D3D11_TEXTURE_ADDRESS_WRAP;
    D3D11_TEXTURE_ADDRESS_MODE addressV = D3D11_TEXTURE_ADDRESS_WRAP;
    D3D11_TEXTURE_ADDRESS_MODE addressW = D3D11_TEXTURE_ADDRESS_WRAP;
    UINT maxAnisotropy = 16;                          // 16x anisotropic filtering
    float mipLODBias = 0.0f;
    float minLOD = 0.0f;
    float maxLOD = D3D11_FLOAT32_MAX;
    XMFLOAT4 borderColor = {0, 0, 0, 0};
};
```

Sampler states are cached by hash so identical sampling configurations share the same `ID3D11SamplerState`.

## Advanced Properties

Advanced shading models are toggled individually and extend the base PBR model. Each feature adds shader permutation defines when enabled.

### Subsurface Scattering

Simulates light transport through translucent materials (skin, wax, marble).

| Property | Default | Description |
|----------|---------|-------------|
| `subsurfaceEnabled` | false | Enable SSS |
| `subsurfaceColor` | (1, 1, 1) | Scattering color tint |
| `subsurfaceRadius` | 1.0 | Scattering radius (world units) |

### Clearcoat

Adds a secondary specular lobe for lacquered or coated surfaces (car paint, varnished wood).

| Property | Default | Description |
|----------|---------|-------------|
| `clearcoatEnabled` | false | Enable clearcoat layer |
| `clearcoatFactor` | 0.0 | Layer strength [0, 1] |
| `clearcoatRoughness` | 0.0 | Layer roughness [0, 1] |

### Anisotropy

Stretches specular highlights along a direction (brushed metal, hair, silk).

| Property | Default | Description |
|----------|---------|-------------|
| `anisotropyEnabled` | false | Enable anisotropic reflections |
| `anisotropyFactor` | 0.0 | Anisotropy strength |
| `anisotropyDirection` | (1, 0) | Tangent-space direction |

### Transmission

Light passing through thin or translucent surfaces (glass, leaves, thin fabric).

| Property | Default | Description |
|----------|---------|-------------|
| `transmissionEnabled` | false | Enable transmission |
| `transmissionFactor` | 0.0 | Transmission strength [0, 1] |
| `transmissionColor` | (1, 1, 1) | Transmission color tint |

### Sheen

Fabric-like soft highlight at grazing angles (velvet, cloth).

| Property | Default | Description |
|----------|---------|-------------|
| `sheenEnabled` | false | Enable sheen |
| `sheenColor` | (0, 0, 0) | Sheen color |
| `sheenRoughness` | 0.0 | Sheen roughness |

### Iridescence

Thin-film interference effects (soap bubbles, oil slicks, beetle shells).

| Property | Default | Description |
|----------|---------|-------------|
| `iridescenceEnabled` | false | Enable iridescence |
| `iridescenceFactor` | 0.0 | Effect strength [0, 1] |
| `iridescenceIOR` | 1.3 | Thin-film IOR |
| `iridescenceThickness` | 100.0 | Film thickness in nanometers |

## Blend Modes

The `BlendMode` enum controls how a material composites with the framebuffer:

| Mode | Description | Typical use |
|------|-------------|-------------|
| `Opaque` | No blending, full depth write | Solid geometry (default) |
| `AlphaTest` | Binary alpha via `alphaCutoff` | Foliage, fences, hair cards |
| `Transparent` | Standard alpha blending | Glass, windows, UI elements |
| `Additive` | Source added to destination | Particles, fire, glow |
| `Multiply` | Source multiplied with destination | Shadow decals, tinting |
| `Screen` | Inverse multiply (brightening) | Light overlays, bloom |

`CullMode` controls face culling: `None` (double-sided), `Front`, or `Back` (default). The `MaterialRenderState` struct also exposes `depthTest`, `depthWrite`, `castShadows`, `receiveShadows`, `renderQueue`, and `doubleSided`.

## Material Variants

Variants represent named shader permutation sets. Each variant stores a list of preprocessor defines that alter the compiled shader:

```cpp
// Create a variant with specific defines
material->CreateVariant("Wet", {"ENABLE_WETNESS", "RAIN_RIPPLES"});
material->CreateVariant("Snow", {"ENABLE_SNOW_COVERAGE"});

// Switch active variant at runtime
material->SetActiveVariant("Wet");

// Query current permutation
auto defines = material->GetShaderPermutation();
```

Variants enable material-level shader customization without duplicating material assets.

## Code Example

```cpp
// Create and configure a PBR material
auto& matSystem = GetMaterialSystem();
auto mat = matSystem.CreateMaterial("BrushedSteel");

// Set PBR surface properties
PBRProperties pbr;
pbr.albedoColor = {0.9f, 0.9f, 0.92f, 1.0f};
pbr.metallicFactor = 1.0f;
pbr.roughnessFactor = 0.35f;
pbr.indexOfRefraction = 2.5f;
mat->SetPBRProperties(pbr);

// Enable anisotropy for brushed-metal highlights
AdvancedProperties adv;
adv.anisotropyEnabled = true;
adv.anisotropyFactor = 0.8f;
adv.anisotropyDirection = {1.0f, 0.0f};
mat->SetAdvancedProperties(adv);

// Load textures
mat->LoadTexture(MaterialTextureType::Albedo, "textures/steel_albedo.dds", device);
mat->LoadTexture(MaterialTextureType::Normal, "textures/steel_normal.dds", device);
mat->LoadTexture(MaterialTextureType::Metallic, "textures/steel_metallic.dds", device);
mat->LoadTexture(MaterialTextureType::Roughness, "textures/steel_roughness.dds", device);
mat->LoadTexture(MaterialTextureType::Anisotropy, "textures/steel_aniso_dir.dds", device);

// Configure render state
MaterialRenderState state;
state.blendMode = BlendMode::Opaque;
state.cullMode = CullMode::Back;
mat->SetRenderState(state);

// Compile pipeline state (blend, depth-stencil, rasterizer, CB)
mat->CompileMaterial(device);

// Bind for rendering
matSystem.BindMaterial("BrushedSteel");

// Hot-reload: watch for texture changes on disk
matSystem.EnableHotReload(true);
// Call each frame to pick up file changes:
matSystem.UpdateHotReload();
```

## Console Commands

The `MaterialSystem` exposes console integration methods for runtime inspection and editing. These are accessed through the `Console_*` methods on `MaterialSystem`:

| Method | Description |
|--------|-------------|
| `Console_GetMetrics()` | Material count, texture memory, bind stats, load times |
| `Console_ListMaterials()` | List all loaded material names |
| `Console_GetMaterialInfo(name)` | Detailed info for a specific material |
| `Console_ReloadMaterial(name)` | Reload a single material from disk |
| `Console_ReloadAllMaterials()` | Reload every loaded material |
| `Console_CreateVariant(name, variant, defines)` | Create a shader variant |
| `Console_ListMaterialVariants(name)` | List variants for a material |
| `Console_SetMaterialProperty(name, prop, value)` | Set a float property at runtime |
| `Console_SetMaterialColor(name, prop, r, g, b)` | Set a color property at runtime |
| `Console_SetHotReload(enabled)` | Enable/disable hot-reload |
| `Console_SetTextureQuality(quality)` | Set texture quality level |
| `Console_GetTextureMemoryInfo()` | Texture memory usage breakdown |
| `Console_LoadTextureToSlot(name, type, path)` | Load a texture into a material slot |
| `Console_UnloadTextureFromSlot(name, type)` | Unload a texture from a slot |
| `Console_ListTextureTypes()` | List all available texture slot types |
| `Console_ClearCache()` | Clear texture and sampler caches |
| `Console_GarbageCollect()` | Remove unused materials |
| `Console_ValidateMaterials()` | Validate all loaded materials |
| `Console_DumpMaterialDetails(name)` | Dump full material details |
| `Console_ExportMaterial(name, path)` | Export material to file |
| `Console_ImportMaterial(path)` | Import material from file |

## Editor Integration

The **MaterialEditorPanel** (`SparkEditor/Source/Panels/MaterialEditorPanel.h`) provides a node-graph-style visual editor for creating and editing materials. It supports:

- PBR parameter sliders and color pickers grouped by category (Surface, Normal, Emission)
- Texture slot assignment with drag-and-drop
- Render state configuration (blend mode, cull mode, depth settings)
- Live preview with scene lighting
- Shader Graph integration for custom material logic

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/MaterialSystem.h` | Material, MaterialSystem, all data structures |
| `SparkEngine/Source/Graphics/MaterialSystem.cpp` | MaterialSystem lifecycle, CRUD, texture loading |
| `SparkEngine/Source/Graphics/PBRMaterial.cpp` | Material class implementation |
| `SparkEngine/Source/Graphics/PBRMaterialBinding.cpp` | Material binding to the GPU pipeline |
| `SparkEngine/Source/Graphics/PBRMaterialLighting.cpp` | PBR lighting calculations |
| `SparkEngine/Source/Graphics/MaterialConsoleOps.cpp` | Console inspection, listing, validation |
| `SparkEngine/Source/Graphics/MaterialConsoleEdit.cpp` | Console editing, texture, hot-reload |
| `SparkEngine/Source/Graphics/MaterialTextureLoading.cpp` | Texture loading from disk |
| `SparkEngine/Source/Graphics/MaterialLoader.h` | Material file loading interface |
| `SparkEngine/Source/Graphics/MaterialLoader.cpp` | Material file loading implementation |
| `SparkEngine/Source/Graphics/MaterialDefinition.h` | Material definition data structures |
| `SparkEngine/Source/Graphics/MaterialPropertyHandle.h` | Type-safe property handle access |
| `SparkEngine/Source/Graphics/MaterialPropertyHandle.cpp` | Property handle implementation |
| `SparkEngine/Source/Graphics/PersistentMaterialCB.h` | Persistent constant buffer management |
| `SparkEditor/Source/Panels/MaterialEditorPanel.h` | Editor panel header |
| `SparkEditor/Source/Panels/MaterialEditorPanel.cpp` | Editor panel implementation |
| `SparkEditor/Source/Panels/MaterialEditorParameters.cpp` | Parameter editing UI |
| `SparkEditor/Source/Panels/MaterialEditorPreview.cpp` | Live preview rendering |

## See Also

- [Shader Graph](Shader-Graph.md) — Node-based shader authoring
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — Shader compilation and management
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Graphics engine overview
