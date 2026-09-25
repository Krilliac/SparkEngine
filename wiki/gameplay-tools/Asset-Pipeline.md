# Asset Pipeline

SparkEngine's `AssetPipeline` wires runtime loading, streaming, caching, and management for mesh, texture, and WAV audio assets. It provides synchronous and asynchronous entry points, an LRU cache with configurable memory budgets, hot reloading for development, and background streaming threads. Other asset kinds named by its enums and data structures are schema surfaces, not implemented `LoadAsset()` branches.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. Source presence and format parsing do not certify a format, backend, or authoring workflow for release use.

**Source:** `SparkEngine/Source/Graphics/AssetPipeline.h`

## Blender MMO source checkpoint

The sixteen `Assets/Models/MMO` props now have distinct Blender-authored
geometry, UVs, normals and same-name material libraries, with original paths
and bounds preserved. Editable source, pinned authoring instructions and
SHA-256 provenance are in [`Art/Blender/MMO`](../../Art/Blender/MMO/README.md).
The legacy generator validates these exports before any writes and preserves
them. CTest registers source-contract and generator regressions; sixteen
production CPU importer cases verify the complete kit. This is a focused
asset-quality checkpoint, not Windows/D3D11, installed-content or release
qualification. The wider [baseline audit](../../Art/Blender/README.md) still
requires visual and contextual review of the remaining assets.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Game Code                                   │
│ LoadMesh() / LoadTexture() / LoadAudio() / LoadAssetAsync()          │
├──────────────────────────────┬──────────────────────────────────────┤
│       AssetPipeline          │         Console Integration           │
│  (orchestrator, metrics,     │   Console_ListAssets()                │
│   hot reload, discovery)     │   Console_GetAssetInfo()              │
├──────────────────────────────┤   Console_LoadAsset()                 │
│                              │   Console_ForceGC()                   │
│   Sync Loading   Async Queue │   Console_SetCacheSize()              │
│       │              │       ├──────────────────────────────────────┤
│       v              v       │                                      │
│   ┌─────────────────────┐    │                                      │
│   │  Format Loaders     │    │                                      │
│   │  OBJ mesh path      │    │                                      │
│   │  native FBXImporter │    │                                      │
│   │  cgltf static mesh  │    │                                      │
│   │  texture loaders    │    │                                      │
│   │  WAV loader         │    │                                      │
│   └─────────┬───────────┘    │                                      │
│             v                │                                      │
│   ┌─────────────────────┐    │                                      │
│   │  AssetCache (LRU)   │    │                                      │
│   │  m_maxMemory (512MB)│    │                                      │
│   │  hit/miss tracking  │    │                                      │
│   └─────────────────────┘    │                                      │
│             v                │                                      │
│   ┌─────────────────────┐    │                                      │
│   │  D3D11 GPU Upload   │    │                                      │
│   │  Vertex/Index Bufs  │    │                                      │
│   │  Texture SRVs       │    │                                      │
│   └─────────────────────┘    │                                      │
└──────────────────────────────┴──────────────────────────────────────┘
```

### Core Types

| Type | Responsibility |
|------|---------------|
| `AssetPipeline` | Main system: loading, caching, streaming, hot reload, metrics |
| `Asset` | Abstract base class for all loaded assets |
| `MeshAsset` | Loaded mesh with vertex/index buffers on GPU |
| `TextureAsset` | Loaded texture with SRV on GPU |
| `AudioAsset` | Loaded audio data (PCM samples) |
| `AssetCache` | LRU eviction cache with configurable memory budget |
| `AssetMetadata` | Per-asset metadata: GUID, path, size, checksum, dependencies |
| `AssetLoadRequest` | Async load request with callbacks |
| `MeshAssetData` | CPU-side mesh data (vertices, indices, bounds) |
| `AnimationAssetData` | Animation-shaped CPU data schema; no `LoadAsset()` branch or model-import handoff currently consumes it |

Only `MeshAsset`, `TextureAsset`, and `AudioAsset` are constructed by the Windows and Linux `AssetPipeline::LoadAsset()` switches. The remaining enum values and data schemas are placeholders until a loader and runtime handoff are wired.

## Enums

### AssetType

| Value | Description |
|-------|-------------|
| `Unknown` | Type not yet determined |
| `Mesh` | 3D model geometry |
| `Texture` | 2D texture image |
| `Material` | Material definition placeholder (not loaded by `LoadAsset()`) |
| `Audio` | Sound data |
| `Animation` | Animation clip placeholder (not loaded by `LoadAsset()`) |
| `Prefab` | Entity prefab template placeholder (not loaded by `LoadAsset()`) |
| `Scene` | Scene definition placeholder (not loaded by `LoadAsset()`) |
| `Shader` | Shader program placeholder (not loaded by `LoadAsset()`) |
| `Font` | Font asset placeholder (not loaded by `LoadAsset()`) |

### LoadingPriority

| Value | Description | Use Case |
|-------|-------------|----------|
| `Low` | Background loading, not time-sensitive | Distant LODs, preloading |
| `Normal` | Standard priority | Most game assets |
| `High` | Load soon, needed for gameplay | Weapons, nearby enemies |
| `Critical` | Load immediately, block if needed | Player model, UI textures |

### StreamingState

| Value | Description |
|-------|-------------|
| `Unloaded` | Asset not in memory |
| `Loading` | Currently being loaded |
| `Loaded` | Fully loaded and usable |
| `Failed` | Load failed (file not found, corrupt, etc.) |
| `Evicted` | Was loaded but evicted from cache |

## Implemented Format Paths

These entries describe observed source paths, not a stable-v1 support matrix.

### 3D Models

| Format | Description | Library | Loader Method |
|--------|-------------|---------|---------------|
| `.obj` | Static Wavefront OBJ geometry | tinyobjloader in the general mesh/scene and non-Windows paths; a limited parser in the Windows `MeshAsset` path | `MeshAsset::Load()` / `LoadOBJ()` |
| `.fbx` | Native binary FBX parsing; the non-Windows mesh path consumes geometry only | `FBXImporter` (no external FBX SDK) | Native importer source; not wired into the Windows stable-v1 `MeshAsset` path |
| `.gltf` / `.glb` | Validated static triangle geometry, or (non-Windows `MeshAsset` only) one skin with four influences per vertex; morph targets, sparse accessors, and required extensions are rejected | cgltf | `LoadGLTFStaticMesh()` / `LoadGLTFSkinnedMesh()` |

### Textures

| Format | Description | Library |
|--------|-------------|---------|
| `.png` | Lossless compressed | stb_image |
| `.jpg` | Lossy compressed | stb_image |
| `.tga` | Targa | stb_image |
| `.bmp` | Bitmap | stb_image |
| `.hdr` | High Dynamic Range | stb_image |

### Audio

| Format | Description | Library |
|--------|-------------|---------|
| `.wav` | RIFF/WAVE runtime asset loading | WAV loader; playback factory selects XAudio2 on Windows, OpenAL on non-Windows, then Null |

### Adjacent Scenes and Data

These formats are handled by other subsystems or represented by schema; they are not additional `AssetPipeline::LoadAsset()` branches.

| Format | Description |
|--------|-------------|
| `.scene` / `.json` | JSON scene files |
| `.prefab` | Prefab templates |
| `.snav` | Binary NavMesh data |

### Shaders

| Format | Description |
|--------|-------------|
| `.hlsl` | HLSL shader source |
| `.glsl` | GLSL shader source |
| `.cso` | Compiled DirectX bytecode |
| `.spv` | SPIR-V bytecode |

## Directory Conventions

### Repository integrity baseline

`Assets/assets.integrity.json` records SHA-256 hashes for the first-party
`Assets/` tree. Git treats `Assets/**` as opaque bytes (`-text`), except
`.obj` and `.mtl` files which are text-normalized to LF (`text eol=lf`).
The manifest hashes describe the checkout bytes after normalization. Verify
the repository baseline and template lock completeness with:

```bash
python3 tools/asset-integrity/verify_asset_integrity.py check-all --repo-root .
python3 tools/asset-integrity/verify_asset_integrity.py verify Assets/assets.integrity.json --root Assets
```

The explicit `--root` is authoritative; manifest metadata cannot redirect the
scan. Verification rejects traversal and Windows path aliases, reparses,
non-regular files, incomplete declarations, resource-limit violations, and a
file that changes while it is read. This is a repository-content gate, not yet
proof that the same verified snapshot was consumed by package assembly.

### stable-v1 package asset profile (OD-09)

The stable-v1 package ships only the runtime asset closure of its in-profile
game modules, not every root under `Assets/` (RDY-020). The focused FPS
shipping selector (`SPARK_GAME_MODULES=SparkGameFPS`, the `windows-shipping`
preset) installs runtime assets through `cmake/SparkRuntimeAssets.cmake`,
which derives the stable-v1 manifest at configure time, skips every file
outside it, and installs the derived manifest as
`bin/Assets/assets.integrity.json`. Other selectors keep the full manifest
(`default` profile).

`tools/asset-integrity/package_closure.py` derives the closure; nothing in it
is hand-listed per asset:

1. The in-profile modules are the `GameModules/module-content-inventory.json`
   modules whose `profileApplicability["stable-v1"]` is `required`. The
   reviewed definition in `tools/asset-integrity/package-profiles.json` must
   name the same modules, so a module joining the profile forces a review.
2. Every asset-rooted string literal in those modules' sources, and in the
   reviewed `engineSources` directory (`SparkEngine/Source/`), is a
   reference. The engine scan is what adds `Models/Cube.obj` and the other
   primitive defaults: `CubeObject.h` and its siblings name them, and FPS and
   SceneManager (for `level1.scene` plane/wall/cube nodes) create those
   objects. Comments and preprocessor lines (`#include "Audio/..."`) are
   skipped.
3. `package-profiles.json` adds reviewed seeds, each with a reason: the
   asset README and `Engine/Branding/` (the startup splash).
4. References are followed transitively. A `.scene` contributes its
   `key=value` asset paths (a bare `model=crate.obj` resolves below
   `Models/`). A material or data `.json` contributes its string values. OBJ
   `mtllib` lines are not followed, because no engine or FPS OBJ loader opens
   MTL files.

A literal is a reference when its first path component names a top-level
asset directory, ignoring case, after backslashes become `/` and a leading
`./` or `Assets/` is dropped. So `models/crate.obj` and `Models\crate.obj`
are both references. Each reference must then name a manifest entry with its
exact case. Configure fails when a reference resolves to nothing or differs
only in case (a typo a case-insensitive Windows filesystem would hide), or
when the closure needs an entry whose license is `NOASSERTION`. Under OD-09
that content cannot ship, and dropping it silently would ship a package that
cannot load its own scene. The only exemptions are the reviewed
`unshippedReferences` in `package-profiles.json`, each with a reason. These
are the DecalSystem `.dds` names that no code opens, and the
EntityPresetManager preset strings that only the editor reads. An exemption
that no scanned source uses any more, or that names a declared asset, also
fails.

Configure re-runs when the profile definition, the module inventory, a
followed scene or material, or the set of files in a scanned source directory
changes. It does not re-run when you edit the contents of an existing source
file, because that would turn every engine edit into a full configure. A
stale closure is caught instead: `verify --profile stable-v1` re-derives it
from the current sources and fails with `profile-incomplete`.

The closure is currently 40 of 883 entries: `Scenes/level1.scene`, its five
materials and their textures, the FPS models and music, the six engine
primitive OBJs, and the branding. It covers the literal asset paths in the
scanned code. It does not cover paths the runtime builds from non-literal
parts. It is not yet proven to be everything the runtime opens: the Linux
proof ran under NullRHI, which creates no meshes, so the D3D11 package smoke
(MOD-310) still has to confirm it.
The `NOASSERTION` TERRAFRONT content and the rest of the TERRAFRONT/MMO
content stay in the repository and in the `default` package.

```bash
# Derive the stable-v1 closure, check it ships no NOASSERTION entry
python3 tools/asset-integrity/verify_asset_integrity.py check-all --profile stable-v1 --strict-provenance
# Derive the stable-v1 manifest and install exclusions (what CMake runs)
python3 tools/asset-integrity/verify_asset_integrity.py package-profile Assets/assets.integrity.json \
    --profile stable-v1 --output stable.json --exclusions excluded.txt --repo-root .
# Check an installed stable-v1 tree
python3 tools/asset-integrity/verify_asset_integrity.py verify <pkg>/bin/Assets/assets.integrity.json \
    --root <pkg>/bin/Assets --profile stable-v1
```

With `--profile`, `check-all --strict-provenance` judges only the entries
that profile ships. Without `--profile` it still fails while any repository
entry is `NOASSERTION`.

`verify --profile stable-v1` fails on any `NOASSERTION` entry
(`profile-excluded`), on an entry the repository manifest records as
`NOASSERTION` even if the package relabels it, on a file the repository
manifest does not declare (`profile-unreviewed`), and on an entry that
differs from the repository entry (`profile-mismatch`). It also fails on a
file outside the derived closure (`profile-outside-closure`) or a closure
file the package omits (`profile-incomplete`). When the source manifest is
not inside a checkout that holds the profile definitions, the check derives
the closure from the verifier's own checkout. If neither has the
definitions, it fails with `profile-closure` rather than skipping the check.
Any file not in the installed manifest fails as `undeclared`. The installed FPS package smoke and
the release workflow's extracted-package check
(`--package-profile ${{ matrix.profile }}`) run it. For stable-v1,
`cmake/ValidateStagedPackageExecutables.cmake` requires
`bin/Assets/Scenes/level1.scene` in place of the TERRAFRONT
`MMOFPS/Data/continents.json`. `PackageAssets_StableV1ExcludesNoAssertion`
(Tests/Tools/test_asset_package_profile.py) covers the closure, the
derivation, the check, and the install rules.

```
Assets/
├── Models/          # 3D model files (.obj, .fbx, .gltf)
├── Scenes/          # Scene files (.scene, .json)
├── Scripts/         # AngelScript files (.as)
├── Textures/        # Texture files (.png, .jpg, .tga)
├── Audio/           # Sound files (.wav)
├── NavMeshes/       # Navigation mesh files (.snav)
├── Prefabs/         # Prefab templates (.prefab)
└── Cinematics/      # Cinematic sequences (.seq)

Shaders/
├── HLSL/            # DirectX shaders
├── GLSL/            # OpenGL shaders
└── Compiled/        # Pre-compiled bytecode
```

## Data Structures

### MeshAssetData

```cpp
struct MeshAssetData
{
    struct Vertex
    {
        XMFLOAT3 position;       // World-space position
        XMFLOAT3 normal;         // Surface normal
        XMFLOAT3 tangent;        // Tangent for normal mapping
        XMFLOAT2 texCoord0;      // Primary UV channel
        XMFLOAT2 texCoord1;      // Secondary UV channel (lightmaps)
        XMFLOAT4 color;          // Vertex color
        XMUINT4 boneIndices;     // Skeletal bone indices (up to 4 bones)
        XMFLOAT4 boneWeights;    // Bone influence weights
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> submeshes;      // Submesh start indices
    XMFLOAT3 boundingBoxMin;              // AABB minimum
    XMFLOAT3 boundingBoxMax;              // AABB maximum
    float boundingSphereRadius;            // Bounding sphere radius
    XMFLOAT3 boundingSphereCenter;         // Bounding sphere center
};
```

The declared fields cover:

- Static meshes (position, normal, tangent, UVs, color)
- Skeletal meshes (adds boneIndices and boneWeights for up to 4 bone influences)
- Lightmapped meshes (uses texCoord1 for lightmap UVs)

### AnimationAssetData

`AnimationAssetData` is a data schema only. The current `AssetPipeline` does not load `AssetType::Animation`, and the model format paths do not convert or register this structure with `AnimationManager`.

```cpp
struct AnimationAssetData
{
    struct Keyframe
    {
        float time;               // Time in seconds
        XMFLOAT3 position;       // Translation
        XMFLOAT4 rotation;       // Quaternion rotation
        XMFLOAT3 scale;          // Scale
    };

    struct AnimationTrack
    {
        std::string boneName;             // Target bone name
        std::vector<Keyframe> keyframes;  // Keyframes sorted by time
    };

    std::string name;                     // Clip name (e.g. "walk", "idle")
    float duration;                       // Total duration in seconds
    float ticksPerSecond;                 // Animation sample rate
    std::vector<AnimationTrack> tracks;   // Per-bone keyframe tracks
};
```

### AssetMetadata

```cpp
struct AssetMetadata
{
    std::string guid;                      // Unique asset identifier
    std::string filePath;                  // Original file path
    std::string name;                      // Human-readable name
    AssetType type;                        // Asset type enum
    size_t fileSize;                       // File size in bytes
    size_t memorySize;                     // GPU/CPU memory footprint
    uint64_t lastModified;                 // File modification timestamp
    std::string checksum;                  // Content hash for change detection
    std::vector<std::string> dependencies; // Other assets this depends on
    LoadingPriority priority;              // Loading priority
    StreamingState state;                  // Current streaming state
    std::unordered_map<std::string, std::string> customProperties;
};
```

### AssetLoadRequest

```cpp
struct AssetLoadRequest
{
    std::string assetPath;                            // File path to load
    AssetType expectedType;                           // Expected type (for validation)
    LoadingPriority priority;                         // Queue priority
    std::function<void(std::shared_ptr<void>)> onLoaded;   // Success callback
    std::function<void(const std::string&)> onError;       // Error callback
    bool blocking = false;                            // Block calling thread until done
};
```

## Asset Base Class

All asset types derive from `Asset`:

```cpp
class Asset
{
public:
    Asset(const std::string& path, AssetType type);
    virtual ~Asset() = default;

    const std::string& GetPath() const;
    AssetType GetType() const;
    bool IsLoaded() const;
    const AssetMetadata& GetMetadata() const;

    virtual HRESULT Load(ID3D11Device* device) = 0;
    virtual void Unload() = 0;
    virtual size_t GetMemoryUsage() const = 0;

protected:
    std::string m_path;
    AssetType m_type;
    bool m_loaded;
    AssetMetadata m_metadata;
};
```

### MeshAsset

```cpp
class MeshAsset : public Asset
{
public:
    MeshAsset(const std::string& path);

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    const MeshAssetData& GetMeshData() const;
    ID3D11Buffer* GetVertexBuffer() const;
    ID3D11Buffer* GetIndexBuffer() const;
    uint32_t GetVertexCount() const;
    uint32_t GetIndexCount() const;
};
```

Holds both CPU-side `MeshAssetData` and GPU-side `ComPtr<ID3D11Buffer>` for vertex and index buffers.

### TextureAsset

```cpp
class TextureAsset : public Asset
{
public:
    TextureAsset(const std::string& path);

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    ID3D11ShaderResourceView* GetSRV() const;
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
};
```

Holds `ComPtr<ID3D11Texture2D>` and `ComPtr<ID3D11ShaderResourceView>` for GPU texture access.

### AudioAsset

```cpp
class AudioAsset : public Asset
{
public:
    AudioAsset(const std::string& path);

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    const std::vector<uint8_t>& GetAudioData() const;
    uint32_t GetSampleRate() const;
    uint32_t GetChannels() const;
    uint32_t GetBitsPerSample() const;
};
```

Stores raw PCM audio data in a `std::vector<uint8_t>`.

## AssetCache

LRU eviction cache for managing loaded assets within a memory budget:

```cpp
class AssetCache
{
public:
    AssetCache(size_t maxMemoryMB = 512);

    void SetMaxMemory(size_t maxMemoryMB);
    size_t GetMaxMemory() const;
    size_t GetCurrentMemory() const;

    void AddAsset(std::shared_ptr<Asset> asset);
    std::shared_ptr<Asset> GetAsset(const std::string& path);
    void RemoveAsset(const std::string& path);
    void EvictLRU();                       // Evict least-recently-used asset
    void Clear();                          // Remove all cached assets

    // Statistics
    uint32_t GetCacheHits() const;
    uint32_t GetCacheMisses() const;
    float GetHitRatio() const;
};
```

### Cache Internals

```cpp
struct CacheEntry  // (private)
{
    std::shared_ptr<Asset> asset;
    uint64_t lastAccessed;     // Timestamp of last access
    size_t accessCount;        // Total access count
};
```

The cache uses `std::unordered_map<std::string, CacheEntry>` keyed by asset path. When memory exceeds `m_maxMemory`, `EvictLRU()` removes the least-recently-accessed entry.

| Default | Value | Description |
|---------|-------|-------------|
| Max memory | 512 MB | Configurable via `SetMaxMemory()` |
| Eviction policy | LRU | Least Recently Used based on `lastAccessed` |

## AssetPipeline API

### Initialization and Lifecycle

```cpp
AssetPipeline pipeline;
HRESULT hr = pipeline.Initialize(device, context);
pipeline.Update(deltaTime);  // Call each frame for async loading + hot reload
pipeline.Shutdown();
```

### Synchronous Loading

```cpp
// Generic load (auto-detect type)
auto asset = pipeline.LoadAsset("Assets/Models/crate.obj");

// Type-specific loads
auto mesh    = pipeline.LoadMesh("Assets/Models/character.fbx");
auto texture = pipeline.LoadTexture("Assets/Textures/brick_albedo.png");
auto audio   = pipeline.LoadAudio("Assets/Audio/explosion.wav");
```

### Asynchronous Loading

```cpp
// Generic async with callbacks
AssetLoadRequest request;
request.assetPath = "Assets/Models/large_building.fbx";
request.expectedType = AssetType::Mesh;
request.priority = LoadingPriority::Normal;
request.onLoaded = [](std::shared_ptr<void> asset) { /* ready */ };
request.onError = [](const std::string& error) { LOG_ERROR(error); };
pipeline.LoadAssetAsync(request);

// Type-specific async
pipeline.LoadMeshAsync("Assets/Models/enemy.fbx",
    [](std::shared_ptr<MeshAsset> mesh) {
        // Mesh is ready for rendering
    });

pipeline.LoadTextureAsync("Assets/Textures/terrain.png",
    [](std::shared_ptr<TextureAsset> tex) {
        // Texture is ready
    });
```

### Asset Management

```cpp
// Query
auto asset = pipeline.GetAsset("Assets/Models/crate.obj");
bool loaded = pipeline.IsAssetLoaded("Assets/Models/crate.obj");

// Unload
pipeline.UnloadAsset("Assets/Models/crate.obj");
pipeline.UnloadAllAssets();
```

### Cache Management

```cpp
pipeline.SetCacheSize(1024);          // Set cache to 1024 MB
pipeline.EvictUnusedAssets();         // Remove assets with zero references

// Preload a batch of assets (async)
pipeline.PreloadAssets({
    "Assets/Models/weapon_rifle.fbx",
    "Assets/Textures/weapon_rifle_albedo.png",
    "Assets/Textures/weapon_rifle_normal.png",
    "Assets/Audio/rifle_fire.wav"
});
```

### Background Streaming

```cpp
pipeline.EnableBackgroundStreaming(true);
pipeline.SetStreamingThreadCount(4);    // 4 worker threads

bool streaming = pipeline.IsBackgroundStreamingEnabled();
int threads = pipeline.GetStreamingThreadCount();
```

### Rendering Helpers

```cpp
// Bind and draw a mesh
pipeline.BindMesh("Assets/Models/crate.obj");
pipeline.BindMaterial("Assets/Materials/crate_mat.json");
pipeline.DrawBoundMesh();
```

### Asset Discovery

```cpp
// Scan a directory for assets of a specific type
auto meshFiles = pipeline.ScanDirectory("Assets/Models/", AssetType::Mesh);
// Returns: ["Assets/Models/crate.obj", "Assets/Models/character.fbx", ...]

// Auto-detect asset type from file extension
AssetType type = pipeline.DetectAssetType("Assets/Textures/brick.png");
// Returns: AssetType::Texture
```

### Metadata

```cpp
AssetMetadata meta = pipeline.GetAssetMetadata("Assets/Models/crate.obj");
// meta.guid, meta.fileSize, meta.memorySize, meta.checksum, meta.dependencies

pipeline.RefreshAssetMetadata("Assets/Models/crate.obj");  // Re-scan file
```

### Hot Reloading

```cpp
pipeline.EnableHotReloading(true);

// Called internally by Update(), but can be triggered manually:
pipeline.CheckForChangedAssets();
```

When enabled, `Update()` periodically checks file timestamps against `m_fileTimestamps`. Changed files are automatically reloaded. Supports:

- Texture files (.png, .jpg, .tga, .bmp, .hdr)
- [Shader source files](Shader-Pipeline.md) (.hlsl, .glsl)
- [AngelScript files](../subsystems/Scripting-with-AngelScript.md) (.as)
- Scene files (.scene, .json)

### Metrics

```cpp
struct AssetMetrics
{
    uint32_t totalAssets;          // Total registered assets
    uint32_t loadedAssets;         // Currently loaded in memory
    uint32_t pendingRequests;      // Waiting in the load queue
    uint32_t failedLoads;          // Failed load attempts
    size_t memoryUsage;            // Current memory usage (bytes)
    size_t maxMemoryUsage;         // Peak memory usage (bytes)
    float averageLoadTime;         // Average load time (ms)
    float cacheHitRatio;           // Cache hit ratio (0.0 - 1.0)
    uint32_t streamingThreads;     // Active streaming threads
    bool backgroundLoading;        // Background loading enabled
};

AssetMetrics metrics = pipeline.GetMetrics();
```

## Internal Implementation

### Loading Thread

When background streaming is enabled, worker threads run `LoadingThreadFunction()` in a loop:

```
LoadingThreadFunction():
  while (!m_shouldStop):
    lock(m_queueMutex)
    wait(m_queueCondition) until queue non-empty or stop
    pop request from m_loadQueue
    unlock

    load asset from disk (CPU)
    upload to GPU (may need main thread for D3D11)

    invoke request.onLoaded or request.onError callback
    update metrics
```

### Type Detection

`DetectAssetTypeFromExtension()` maps file extensions to `AssetType`:

| Extensions | AssetType |
|-----------|-----------|
| `.obj`, `.fbx`, `.gltf`, `.glb` | `Mesh` |
| `.png`, `.jpg`, `.tga`, `.bmp`, `.hdr` | `Texture` |
| `.wav` | `Audio` |
| `.hlsl`, `.glsl`, `.cso`, `.spv` | `Shader` |
| `.scene`, `.json` | `Scene` |
| `.prefab` | `Prefab` |
| `.anim` | `Animation` |

### Hot Reload Detection

```
CheckForChangedAssets():
  for each (path, oldTimestamp) in m_fileTimestamps:
    newTimestamp = GetFileTimestamp(path)
    if newTimestamp != oldTimestamp:
      reload asset at path
      update m_fileTimestamps[path]
```

### Memory Management

The pipeline tracks memory at two levels:

1. **Per-asset**: Each `Asset` subclass reports `GetMemoryUsage()` (GPU + CPU)
2. **Cache-level**: `AssetCache` sums all entries and enforces the memory budget

When the cache exceeds its budget, `EvictLRU()` removes the least-recently-accessed assets that have no active `shared_ptr` references outside the cache.

## Texture Quality Levels

| Level | Description |
|-------|-------------|
| Low | Quarter resolution, basic filtering |
| Medium | Half resolution, bilinear filtering |
| High | Full resolution, anisotropic filtering |
| Ultra | Full resolution, max anisotropic filtering |

## Model Loading Details

### OBJ Files (tinyobjloader)

Best for simple static meshes with basic materials:

```cpp
auto mesh = pipeline.LoadMesh("Assets/Models/crate.obj");
// Parses: vertex positions, normals, texture coordinates
// Generates: tangents, bounding box/sphere
```

### FBX Files (Native Importer)

`FBXImporter` is a native binary FBX parser with no external FBX SDK dependency. Its result type exposes meshes, bones, and animations, but the current `MeshAsset` integration validates without requiring a skeleton or animation and copies only geometry on the non-Windows path. The Windows `MeshAsset` path has no FBX branch.

There is no current conversion or registration handoff from `FBXImportResult` to `AnimationManager`. End-to-end FBX skeletal or animation ingestion is therefore absent from this pipeline rather than an implied feature of the result structures.

### glTF 2.0 Static Meshes (cgltf)

```cpp
auto mesh = pipeline.LoadMesh("Assets/Models/weapon.gltf");
// Parses validated static triangle geometry: POSITION, NORMAL,
// TEXCOORD_0, and optional unsigned indices.
```

`LoadGLTFStaticMesh()` is deliberately a static-mesh subset. It rejects skins, animations, morph targets, sparse accessors, non-triangle primitives, and required extensions; it does not import PBR material or texture graphs.

The canonical Blender-authored static fixture lives in `Tests/Fixtures/GLTFStaticMesh/BlenderBox/` with an editable compressed `.blend`, reproducible author/export script, GLB, and hash-bound provenance. `GLTFStaticMesh_LoadsBlenderAuthoredStaticBox` exercises the production CPU loader against its nonuniform applied-transform box: exported axis-converted bounds, flat normals, all six faces' geometric-corner/UV associations, triangle winding/area, and indices. This establishes the documented static attribute contract only; D3D11 rendering, scene-node transforms, skeletal data, animation, and packaged-content certification remain separate requirements.

### glTF 2.0 Skins (cgltf, CPU importer)

`LoadGLTFSkinnedMesh()` (`Graphics/GLTFSkinnedMeshLoader.h`) is the fail-closed CPU importer for one glTF skin. It reads `POSITION`, `NORMAL`, optional `TEXCOORD_0`, `JOINTS_0`, and `WEIGHTS_0` into vertices with exactly four influences, plus a `Spark::Animation::Skeleton` whose bones are ordered parent-first (a stable topological order, so joints already listed parent-first keep their indices). Vertex joint indices are remapped to that bone order. `offsetMatrix` is the skin's inverse bind matrix (identity when absent) and `localBindPose` is the joint node's local transform; the root bone also folds in every non-joint ancestor (for example the Blender armature object). Matrices are the glTF column-major array read as DirectXMath row-major, so the translation sits in `_41.._43`.

It rejects, with a diagnostic naming the node, joint, primitive, or vertex:

- `JOINTS_1`/`WEIGHTS_1` (more than four influences) and any attribute outside the five above; a missing `NORMAL`
- `JOINTS_0` that is not a non-normalized unsigned byte/short `VEC4`, and `WEIGHTS_0` that is not float or normalized unsigned byte/short `VEC4`
- joint indices outside the skin (including zero-weight slots), negative or non-finite weights, and zero-sum weights
- weight sums outside `1 +/- kGLTFSkinWeightSumTolerance` (0.01, which covers four quantized UNSIGNED_BYTE weights); sums inside it are renormalized to exactly 1
- more than `kMaxBonesPerMesh` (256, the GPU skinning palette) joints, duplicate joints or joint names, and anything other than exactly one skin
- inverse bind matrices that are not a float `MAT4` accessor with one matrix per joint, or that are non-finite, non-affine, or singular
- cyclic node graphs (detected before cgltf walks any parent chain), joints separated from their parent joint by a non-joint node, joints forming more than one tree, and mesh nodes that do not reference the skin
- everything the static loader rejects (sparse accessors, morph targets, required extensions, oversized or unaligned buffers)

The mesh loader ignores animations; `LoadGLTFAnimationClips()` (`Graphics/GLTFAnimationLoader.h`) imports them for `AnimationManager::LoadAnimations()` (see [Animation](../subsystems/Animation.md#asset-ingestion-boundary)). All glTF loaders share `GLTFValidation.h/.cpp` (root-confined reads, size limits, and buffer/view/accessor pre-validation), so the skinned path cannot relax the static loader's limits; the parsers are one entry in the SEC-120 parser inventory. `GLTF_Skinning_*` tests (ctest `GLTFSkinnedMeshImport`, exact count) build every GLB fixture in-test. The portable (non-Windows) `MeshAsset::Load()` checks whether a `.gltf`/`.glb` declares a skin (`GLTFFileHasSkin()`) and then uses this loader, copying joints and weights into `MeshAssetData::Vertex::boneIndices`/`boneWeights` (indices address the skeleton `AnimationManager::LoadSkeleton()` builds from the same file) and recording `gltf.boneCount` in the asset metadata; an invalid skin fails the load. The Windows D3D11 `MeshAsset` (`AssetTypesWindows.cpp`) still uses only the static loader and rejects skinned glTF. Nothing uploads the bone data to `GPUSkinning` or a skinned shader, and there is no Blender-authored skinned fixture.

### Observed Model Data Handoffs

| Format path | Data handed to `MeshAssetData` | Current limit |
|-------------|--------------------------------|---------------|
| OBJ | Positions, normals, texture coordinates, and triangle indices | Static geometry path; parser details vary by platform/caller |
| Native FBX | Geometry on the non-Windows `MeshAsset` path | No Windows `MeshAsset` branch and no `AnimationManager` handoff |
| cgltf | Static triangle positions, normals, one UV set, and indices; on the non-Windows `MeshAsset` path also four bone indices and weights per vertex for a skinned file | The Windows `MeshAsset` rejects skinned files; no draw path consumes the bone data; animations go to `AnimationManager` instead; morph targets and PBR material graphs are not ingested |

## Error Handling

| Scenario | Behavior |
|----------|----------|
| File not found | Returns `nullptr` (sync) or invokes `onError` callback (async) |
| Corrupt file data | `Load()` returns failure HRESULT; asset marked `StreamingState::Failed` |
| GPU buffer creation fails | `HRESULT` propagated; asset not marked as loaded |
| Cache memory exceeded | LRU eviction triggered automatically |
| Hot reload file changed | Asset reloaded; old GPU resources released via `ComPtr` ref counting |
| Async load thread crash | Thread isolation; error logged, other threads continue |
| Unknown file extension | `DetectAssetType` returns `AssetType::Unknown` |

## Performance Considerations

| Parameter | Default | Description |
|-----------|---------|-------------|
| Cache size | 512 MB | `AssetCache` max memory budget |
| Streaming threads | Platform-dependent | Set via `SetStreamingThreadCount()` |
| Hot reload | Enabled | File timestamp polling each frame |
| Load queue | Priority-ordered | Higher priority requests served first |

### Tips

1. **Preload critical assets** during loading screens with `PreloadAssets()`
2. **Set appropriate priorities**: Use `Critical` for player/weapon assets, `Low` for distant scenery
3. **Monitor cache hit ratio**: Below 0.5 suggests the cache is too small or assets churn too fast
4. **Disable hot reloading** in shipping builds to avoid file timestamp overhead
5. **Use `.gltf`/`.glb` only for the implemented static and single-skin subsets**; validate authoring output against the rejected-feature lists
6. **Batch directory scans** during loading rather than at runtime

## Thread Safety

| Component | Thread Safety | Details |
|-----------|--------------|---------|
| `AssetPipeline` | Partially thread-safe | `m_assetsMutex` protects the asset map; `m_queueMutex` protects the load queue |
| `AssetCache` | Thread-safe | `m_mutex` protects all cache operations |
| `Asset::Load()` | Not thread-safe | D3D11 device calls must happen on the main thread or deferred context |
| Hot reload | Main thread only | `CheckForChangedAssets()` reads file timestamps on the main thread |
| Metrics | Thread-safe | `m_metricsMutex` protects metric reads/writes |

### Async Loading Thread Safety Model

```
Background Thread:                    Main Thread:
  Read file from disk (safe)           pipeline.Update(dt)
  Parse mesh data (safe)                 -> complete pending GPU uploads
  Queue GPU upload request               -> invoke onLoaded callbacks
                                         -> check hot reload timestamps
```

GPU resource creation (`ID3D11Device::CreateBuffer`, `CreateTexture2D`) must be called from the main thread or a deferred context. Background threads handle only CPU-side file I/O and parsing.

## Console Commands

```
asset_list                  # List all loaded assets with type and memory
asset_info <path>           # Show detailed metadata for a specific asset
asset_load <path>           # Load an asset synchronously
asset_unload <path>         # Unload a specific asset
asset_cache_size <MB>       # Set cache size in MB
asset_gc                    # Force garbage collection of unreferenced assets
asset_streaming <on|off>    # Enable/disable background streaming
asset_threads <count>       # Set number of streaming threads
asset_scan <directory>      # Scan directory and report found assets
asset_hot_reload <on|off>   # Enable/disable hot reloading
asset_preload <directory>   # Preload all assets in a directory
asset_reload_all            # Force reload all loaded assets
```

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|---------------|----------|
| Asset returns `nullptr` | File path wrong or file missing | Verify path relative to working directory |
| Texture appears black | SRV creation failed | Check HRESULT from `Load()`; verify D3D11 device |
| Mesh has no vertices | File format not supported or corrupt | Check file extension; try re-exporting from DCC tool |
| Async callback never fires | Background streaming disabled | Call `EnableBackgroundStreaming(true)` |
| Memory usage grows unbounded | Cache not configured | Set appropriate cache size with `SetCacheSize()` |
| Hot reload not working | Hot reloading disabled | Call `EnableHotReloading(true)`; ensure `Update()` called each frame |
| Load takes too long | Too few streaming threads | Increase with `SetStreamingThreadCount()` |
| Cache hit ratio is 0 | Assets loaded but not accessed through cache | Use `GetAsset()` for subsequent accesses |
| FBX skeleton or clips are unavailable | No current importer-to-`AnimationManager` handoff | A separately verified conversion tool would be required to produce `.skel`/`.sanim`; this pipeline does not provide that conversion |
| glTF skin data is missing on Windows, or morph/material data is unavailable | The Windows `MeshAsset` loads the static subset only; morph targets and material graphs are not imported | Use the non-Windows `MeshAsset` or `AnimationManager::LoadSkeleton()` for skins; export morph and material data through separately wired systems |

## Utility Functions

```cpp
std::string AssetTypeToString(AssetType type);           // "Mesh", "Texture", etc.
AssetType StringToAssetType(const std::string& str);     // Reverse lookup
std::string StreamingStateToString(StreamingState state); // "Loaded", "Failed", etc.
std::string LoadingPriorityToString(LoadingPriority p);   // "Low", "Normal", etc.
```

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Material and texture systems
- [Animation](../subsystems/Animation.md) -- Runtime `.skel`/`.sanim` asset boundary
- [Shader Pipeline](Shader-Pipeline.md) -- Shader compilation
- [Scene Management](../subsystems/Scene-Management.md) -- Scene file loading
- [Audio](../subsystems/Audio.md) -- Audio asset formats and loading
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation.md) -- Terrain asset streaming
- [Entity Component System](../subsystems/Entity-Component-System.md) -- Component-based asset references
