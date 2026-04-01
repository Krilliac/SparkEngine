# SparkEngine Asset Format Specification

**Version:** 1.0  
**Date:** 2026-04-01  
**Status:** Reference  

## Overview

SparkEngine's `AssetPipeline` manages loading, caching, streaming, and lifecycle of all game assets. Assets are loaded asynchronously with priority-based scheduling and LRU eviction.

## Asset Types

| Type | Enum Value | Extensions | Description |
|------|-----------|------------|-------------|
| `Unknown` | 0 | — | Unrecognized file type |
| `Mesh` | 1 | `.obj`, `.fbx`, `.gltf`, `.glb` | 3D model geometry |
| `Texture` | 2 | `.png`, `.jpg`, `.bmp`, `.tga`, `.dds`, `.hdr`, `.exr` | Image data |
| `Material` | 3 | `.sparkmat`, `.mat` | PBR material definitions |
| `Audio` | 4 | `.wav`, `.ogg`, `.mp3`, `.flac` | Sound effects and music |
| `Animation` | 5 | `.sparkanim`, `.anim` | Skeletal animation clips |
| `Prefab` | 6 | `.archetype`, `.prefab` | Entity templates with components |
| `Scene` | 7 | `.sparkscene`, `.scene` | Level/world definitions |
| `Shader` | 8 | `.hlsl`, `.glsl`, `.spv` | GPU shader programs |
| `Font` | 9 | `.ttf`, `.otf` | TrueType/OpenType fonts |

**Detection:** File type is determined by extension (case-insensitive).

## Loading Priority

| Priority | Value | Description | Use Case |
|----------|-------|-------------|----------|
| `Low` | 0 | Background loading | Distant LODs, preloading |
| `Normal` | 1 | Standard priority | Most gameplay assets |
| `High` | 2 | Prioritized loading | Player-visible assets |
| `Critical` | 3 | Immediate loading | UI, player model, weapons |

Higher priority assets are loaded before lower priority ones in the async queue.

## Streaming States

| State | Description |
|-------|-------------|
| `Unloaded` | Asset metadata known, data not in memory |
| `Loading` | Async load in progress |
| `Loaded` | Data in memory, ready for use |
| `Failed` | Load attempt failed (missing file, corrupt data) |
| `Evicted` | Was loaded, removed by LRU cache to free memory |

## LRU Cache

The `AssetCache` manages loaded asset memory:

- **Max memory budget:** Configurable via `SetMaxMemory()` (default: 512MB)
- **Eviction policy:** Least Recently Used — oldest-accessed assets evicted first
- **Eviction trigger:** When total memory exceeds budget
- **Hit/miss tracking:** `GetHitRate()` returns cache efficiency ratio
- **Manual control:** `Remove()` evicts specific assets, `Clear()` empties cache

### Cache Operations

| Operation | Complexity | Description |
|-----------|-----------|-------------|
| `Add(key, asset, size)` | O(1) | Insert asset, update memory tracking |
| `Get(key)` | O(1) | Retrieve asset, update LRU position |
| `Remove(key)` | O(1) | Evict specific asset |
| `EvictLRU(count)` | O(k) | Remove k least-recently-used assets |
| `Clear()` | O(n) | Remove all assets |

## Mesh Asset Format

### Vertex Layout

```cpp
struct Vertex {
    XMFLOAT3 position;      // 12 bytes — world position
    XMFLOAT3 normal;        // 12 bytes — surface normal
    XMFLOAT3 tangent;       // 12 bytes — tangent for normal mapping
    XMFLOAT2 texCoord0;     //  8 bytes — primary UV
    XMFLOAT2 texCoord1;     //  8 bytes — secondary UV (lightmaps)
    XMFLOAT4 color;         // 16 bytes — vertex color
    uint32_t boneIndices[4]; // 16 bytes — skeletal bone indices
    float    boneWeights[4]; // 16 bytes — skeletal bone weights
};
// Total: 100 bytes per vertex
```

### Mesh Data

```cpp
struct MeshAssetData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;  // Material-separated mesh parts
    
    struct BoundingBox {
        XMFLOAT3 min, max;
    } boundingBox;
    
    struct BoundingSphere {
        XMFLOAT3 center;
        float radius;
    } boundingSphere;
};
```

### Supported Mesh Formats

| Format | Library | Platform | Notes |
|--------|---------|----------|-------|
| glTF 2.0 (`.gltf`, `.glb`) | cgltf | All | Preferred format |
| OBJ (`.obj`) | Built-in parser | All | Triangulated only |
| FBX (`.fbx`) | FBX SDK | Windows only | Proprietary SDK required |

## Material Format

Materials use a PBR metallic-roughness workflow.

### Material File Structure (JSON)

```json
{
    "name": "BrickWall",
    "pbrProperties": {
        "albedoColor": [0.8, 0.3, 0.2, 1.0],
        "metallicFactor": 0.0,
        "roughnessFactor": 0.7,
        "normalScale": 1.0,
        "occlusionStrength": 1.0,
        "emissiveColor": [0.0, 0.0, 0.0],
        "emissiveFactor": 0.0,
        "alphaCutoff": 0.5,
        "indexOfRefraction": 1.5
    },
    "renderState": {
        "blendMode": "Opaque",
        "cullMode": "Back",
        "depthTest": true,
        "depthWrite": true,
        "castShadows": true,
        "receiveShadows": true,
        "renderQueue": 2000,
        "doubleSided": false
    },
    "textures": {
        "Albedo": { "path": "textures/brick_albedo.png", "tiling": [1, 1], "offset": [0, 0] },
        "Normal": { "path": "textures/brick_normal.png" },
        "Roughness": { "path": "textures/brick_roughness.png" }
    },
    "variants": {
        "Wet": ["ENABLE_WET_SURFACE", "USE_RAIN_RIPPLES"]
    }
}
```

### Blend Modes

| Mode | Value | Description |
|------|-------|-------------|
| `Opaque` | 0 | Fully opaque, no blending |
| `AlphaTest` | 1 | Binary alpha (cutout) |
| `Transparent` | 2 | Alpha blending |
| `Additive` | 3 | Additive blending (fire, glow) |
| `Multiply` | 4 | Multiplicative blending |
| `Screen` | 5 | Screen blending |

### Texture Slots

| Slot | Description | Default |
|------|-------------|---------|
| `Albedo` | Base color | White |
| `Normal` | Normal map | Flat (0.5, 0.5, 1.0) |
| `Metallic` | Metallic map | 0.0 |
| `Roughness` | Roughness map | 0.5 |
| `Occlusion` | Ambient occlusion | 1.0 |
| `Emissive` | Emissive map | Black |
| `Height` | Displacement/parallax | None |
| `DetailAlbedo` | Detail texture | None |
| `DetailNormal` | Detail normal map | None |
| `Subsurface` | SSS color map | None |
| `Transmission` | Transmission map | None |
| `Clearcoat` | Clearcoat layer | None |
| `ClearcoatRoughness` | Clearcoat roughness | None |
| `Anisotropy` | Anisotropy direction | None |
| `Custom0-3` | Game-specific slots | None |

### Advanced Properties

Materials support advanced PBR extensions:
- **Subsurface scattering:** Color, radius
- **Clearcoat:** Factor, roughness
- **Anisotropy:** Factor, direction
- **Transmission:** Factor, color
- **Sheen:** Color, roughness (fabric)
- **Iridescence:** Factor, IOR, thickness

## Archetype/Prefab Format

Entity templates (`.archetype` files) define spawnable entity configurations:

```json
{
    "name": "EnemySoldier",
    "components": {
        "Transform": {
            "position": [0, 0, 0],
            "rotation": [0, 0, 0],
            "scale": [1, 1, 1]
        },
        "MeshRenderer": {
            "mesh": "models/soldier.glb",
            "material": "materials/soldier.sparkmat",
            "castShadows": true
        },
        "RigidBody": {
            "type": "Dynamic",
            "mass": 80.0,
            "friction": 0.6
        },
        "HealthComponent": {
            "maxHealth": 100
        },
        "AIController": {
            "behaviorTree": "ai/soldier_bt.json"
        }
    },
    "overrides": {
        "Transform.scale": [1.2, 1.2, 1.2]
    }
}
```

**Property overrides** allow instances to customize specific properties without duplicating the entire template.

## Texture Formats

| Format | Extension | Channels | HDR | Compression | Library |
|--------|-----------|----------|-----|-------------|---------|
| PNG | `.png` | RGBA | No | Lossless | stb_image |
| JPEG | `.jpg` | RGB | No | Lossy | stb_image |
| BMP | `.bmp` | RGBA | No | None | stb_image |
| TGA | `.tga` | RGBA | No | RLE | stb_image |
| DDS | `.dds` | RGBA | Both | BC1-BC7 | DirectX |
| HDR | `.hdr` | RGB | Yes | RGBE | stb_image |
| EXR | `.exr` | RGBA | Yes | Various | tinyexr |

### Texture Sampling Parameters

```cpp
struct TextureSampling {
    D3D11_FILTER filter;            // Anisotropic, Linear, Point
    D3D11_TEXTURE_ADDRESS_MODE addressU, addressV, addressW; // Wrap, Clamp, Mirror, Border
    UINT maxAnisotropy;             // 1-16
    float mipLODBias;               // MIP level offset
    float minLOD, maxLOD;           // LOD clamping
    XMFLOAT4 borderColor;          // For border address mode
};
```

## Asset Directories

```
Assets/
├── Models/          — 3D meshes (.glb, .obj, .fbx)
├── Textures/        — Image files (.png, .jpg, .dds, .hdr)
├── Materials/       — Material definitions (.sparkmat)
├── Audio/           — Sound files (.wav, .ogg, .mp3)
├── Animations/      — Animation clips (.sparkanim)
├── Scenes/          — Level files (.sparkscene)
├── Scripts/         — AngelScript files (.as)
├── Shaders/         — GPU shaders (.hlsl)
└── Prefabs/         — Entity templates (.archetype)
```
