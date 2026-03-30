# Asset Format Specifications

This page documents the internal data formats used by SparkEngine's [asset pipeline](Asset-Pipeline) for meshes, textures, materials, audio, and animations.

**Source:** `SparkEngine/Source/Graphics/AssetPipeline.h`, `SparkEngine/Source/Core/AssetHandle.h`, `SparkEngine/Source/Core/AssetIntegration.h`

---

## Asset Types

```cpp
enum class AssetType {
    Unknown, Mesh, Texture, Material, Audio, Animation, Prefab, Scene, Shader, Font
};
```

Each asset has a lifecycle state:

| State | Description |
|-------|-------------|
| `Unloaded` | Metadata known, data not in memory |
| `Loading` | Background thread is loading |
| `Loaded` | Ready for use |
| `Failed` | Load error occurred |
| `Evicted` | Removed from cache (can be reloaded) |

---

## Asset Handle System

Assets are referenced by a 64-bit FNV-1a hash of their file path, avoiding string comparisons in hot paths:

```cpp
struct AssetHandle {
    uint64_t hash;
    static AssetHandle FromPath(std::string_view path);
    bool IsValid() const;
};
```

The `AssetRegistry` provides O(1) lookup by handle:

```cpp
AssetHandle h = AssetHandle::FromPath("Assets/Meshes/Tree.fbx");
MeshAsset* mesh = registry.Get<MeshAsset>(h);
```

---

## Mesh Format

### Vertex Layout

Each vertex contains 14 floats of geometric data plus skeletal animation weights:

```cpp
struct Vertex {
    XMFLOAT3 position;     // 12 bytes — World-space position
    XMFLOAT3 normal;       // 12 bytes — Surface normal
    XMFLOAT3 tangent;      // 12 bytes — Tangent for normal mapping
    XMFLOAT2 texCoord0;    //  8 bytes — Primary UV channel
    XMFLOAT2 texCoord1;    //  8 bytes — Secondary UV (lightmaps)
    XMFLOAT4 color;        // 16 bytes — Vertex color (RGBA)
    XMUINT4  boneIndices;  // 16 bytes — Up to 4 bone influences
    XMFLOAT4 boneWeights;  // 16 bytes — Corresponding weights
};
// Total: 100 bytes per vertex
```

### Mesh Asset Data

```cpp
struct MeshAssetData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;         // Triangle list (3 indices per tri)
    std::vector<uint32_t> submeshes;       // Start index of each submesh
    XMFLOAT3 boundingBoxMin, boundingBoxMax;
    float boundingSphereRadius;
    XMFLOAT3 boundingSphereCenter;
};
```

### Supported Import Formats

| Format | Loader | Platform | Notes |
|--------|--------|----------|-------|
| `.obj` | `tinyobj::LoadObj()` | All | Vertices, normals, UVs. No animation. |
| `.fbx` | FBX SDK | Windows | Full skeletal mesh + animation support. |
| `.gltf`/`.glb` | Planned | — | Not yet implemented. |

### Import Settings

```cpp
struct MeshSettings {
    bool generateNormals = true;
    bool generateTangents = true;
    bool generateLightmapUVs = false;
    float normalSmoothingAngle = 60.0f;
    bool optimizeMesh = true;
    bool weldVertices = true;
    float weldThreshold = 0.0001f;
};
```

---

## Texture Format

Textures are loaded via `stb_image` and uploaded as D3D11 shader resource views.

### GPU Upload

```cpp
class TextureAsset : public Asset {
    ID3D11ShaderResourceView* GetSRV() const;
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
};
```

### Supported Import Formats

| Format | Notes |
|--------|-------|
| `.png` | 8-bit RGBA, recommended for UI/sprites |
| `.jpg` | Lossy, no alpha channel |
| `.bmp` | Uncompressed |
| `.tga` | 24/32-bit |
| `.hdr` | High dynamic range (float) |

### Import Settings

```cpp
struct TextureSettings {
    enum Format { AUTO, DXT1, DXT5, BC7, UNCOMPRESSED };
    int maxTextureSize = 2048;
    bool generateMipMaps = true;
    bool sRGB = true;
    float compressionQuality = 0.8f;
};
```

---

## Audio Format

Audio assets are loaded as raw PCM data for XAudio2 playback.

```cpp
class AudioAsset : public Asset {
    const std::vector<uint8_t>& GetAudioData() const;
    uint32_t GetSampleRate() const;   // e.g., 44100
    uint32_t GetChannels() const;     // 1 = mono, 2 = stereo
    uint32_t GetBitsPerSample() const; // 16 or 32
};
```

### Import Settings

```cpp
struct AudioSettings {
    enum Format { AUTO, WAV, OGG, MP3 };
    int sampleRate = 44100;
    int bitDepth = 16;
    bool force3D = false;
    float compressionQuality = 0.7f;
};
```

---

## Animation Format

Skeletal animations are stored as per-bone keyframe tracks:

```cpp
struct AnimationAssetData {
    struct Keyframe {
        float time;           // Time in seconds
        XMFLOAT3 position;   // Bone-local position
        XMFLOAT4 rotation;   // Quaternion rotation
        XMFLOAT3 scale;      // Bone-local scale
    };

    struct AnimationTrack {
        std::string boneName;
        std::vector<Keyframe> keyframes;
    };

    std::string name;
    float duration;          // Total clip length in seconds
    float ticksPerSecond;    // Keyframe sampling rate
    std::vector<AnimationTrack> tracks;
};
```

### Import Settings

```cpp
struct AnimationSettings {
    bool importAnimation = true;
    bool optimizeKeyframes = true;
    float keyframeReduction = 0.01f;  // Remove keys within this tolerance
};
```

---

## Scene Format

Scenes are stored as JSON files (`.scene` or `.json`):

```json
{
    "metadata": {
        "sceneName": "Level01",
        "author": "LevelDesigner",
        "version": "1.0",
        "ambientLightR": 0.1, "ambientLightG": 0.1, "ambientLightB": 0.1,
        "gravityX": 0.0, "gravityY": -9.81, "gravityZ": 0.0
    },
    "nodes": [
        {
            "name": "Ground",
            "type": "plane",
            "position": [0, 0, 0],
            "rotation": [0, 0, 0],
            "scale": [1, 1, 1],
            "parentIndex": -1,
            "properties": {}
        },
        {
            "name": "Box",
            "type": "cube",
            "position": [0, 1, 0],
            "parentIndex": 0,
            "properties": { "health": "100" }
        }
    ]
}
```

### Node Types

| `type` | Instantiated as |
|--------|-----------------|
| `"cube"` | Unit cube mesh |
| `"sphere"` | Unit sphere mesh |
| `"plane"` | Flat XZ plane |
| `"model"` | External mesh from `modelPath` |
| `"light"` | Dynamic light source |
| `"trigger"` | Invisible trigger volume |

A legacy binary `.scene` format is also supported for older files via `SceneManager::LoadCustom()`.

---

## Asset Metadata

Every asset tracked by the pipeline carries metadata:

```cpp
struct AssetMetadata {
    std::string guid;          // Unique identifier
    std::string filePath;      // Disk path
    std::string name;          // Display name
    AssetType type;
    size_t fileSize;           // On-disk size in bytes
    size_t memorySize;         // GPU/RAM size when loaded
    uint64_t lastModified;     // Filesystem timestamp
    std::string checksum;      // Content hash for cache invalidation
    std::vector<std::string> dependencies;  // Other assets this depends on
    LoadingPriority priority;
    StreamingState state;
};
```

---

## Asset Cache

The pipeline uses an LRU cache with a configurable memory budget (default 512 MB):

```cpp
pipeline.SetCacheSize(1024);  // 1 GB
pipeline.EvictUnusedAssets();  // Force eviction pass
pipeline.PreloadAssets({"Assets/Meshes/Player.fbx", "Assets/Textures/Ground.png"});
```

Hot reloading monitors file timestamps and automatically reloads modified assets during development:

```cpp
pipeline.EnableHotReloading(true);
pipeline.CheckForChangedAssets();  // Called each frame in editor
```

---

## SparkPak Archive Format (.spk)

SparkPak is an MPQ-inspired binary archive format for bundling assets into single files. Archives are optional — the engine loads loose files by default and mounts .spk archives on top via the [VirtualFileSystem](Mod-System).

**Source:** `SparkEngine/Source/Core/SparkPak.h`, `SparkEngine/Source/Core/SparkPakWriter.h`, `SparkEngine/Source/Engine/Modding/ArchiveResourceProvider.h`

### File Layout

```
[Header 32 bytes]  [File data blobs...]  [Compressed TOC at end]
```

| Field | Size | Description |
|-------|------|-------------|
| Magic | 4B | `"SPK1"` (0x314B5053 little-endian) |
| Version | 4B | Format version (currently 1) |
| FileCount | 4B | Number of entries |
| Reserved | 4B | — |
| TOCOffset | 8B | Byte offset of compressed table of contents |
| TOCSize | 4B | Compressed TOC size |
| TOCRawSize | 4B | Uncompressed TOC size |

### TOC Entry (per file)

| Field | Size | Description |
|-------|------|-------------|
| PathHash | 8B | FNV-1a 64-bit hash (matches `AssetHandle`) |
| DataOffset | 8B | Byte offset of file data in archive |
| CompressedSize | 4B | Size of stored data |
| OriginalSize | 4B | Uncompressed size |
| Compression | 1B | 0 = stored, 1 = deflate |
| PathLength | 2B | Length of path string |
| Path | var | Virtual path (for listing/debugging) |

### Features

- **O(1) hash lookup** using FNV-1a (same hash as `AssetHandle`)
- **Per-file deflate compression** via miniz (skipped when savings < 5%)
- **TOC at end of file** (like ZIP central directory) — streaming-friendly
- **Priority layering** via VFS — patch archives override base archives
- **Auto-mount** — `.spk` files in `Data/` directory are mounted at startup

### Usage

```cpp
// Writing an archive
Spark::SparkPakWriter writer;
writer.AddDirectory("Assets/", "");
writer.Finalize("Data/base.spk");

// Reading directly
Spark::SparkPakReader reader;
reader.Open("Data/base.spk");
auto data = reader.ReadFile("textures/brick.png");

// Via VFS (automatic after mounting)
auto& vfs = Spark::VirtualFileSystem::GetInstance();
vfs.Mount("base", std::make_unique<Spark::ArchiveResourceProvider>("Data/base.spk"),
          Spark::ENGINE_PRIORITY);
auto data = vfs.ReadFile("textures/brick.png");
```

### Console Commands

| Command | Description |
|---------|-------------|
| `pak_status` | Show all VFS mount points and mounted archives |
| `pak_list [dir] [.ext]` | List files in VFS, optionally filtered |

See [Asset Pipeline](Asset-Pipeline) for the full pipeline architecture.
