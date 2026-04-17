# Asset Pipeline

SparkEngine's asset pipeline handles loading, streaming, caching, and management of game assets including 3D models, textures, [audio](../subsystems/Audio.md), animations, and [scenes](../subsystems/Scene-Management.md). It provides both synchronous and asynchronous loading, an LRU cache with configurable memory budgets, hot reloading for development, and background streaming threads.

**Source:** `SparkEngine/Source/Graphics/AssetPipeline.h`

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Game Code                                   │
│   LoadMesh() / LoadTexture() / LoadAssetAsync() / PreloadAssets()    │
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
│   │  LoadOBJ()          │    │                                      │
│   │  LoadFBX()          │    │                                      │
│   │  LoadGLTF()         │    │                                      │
│   │  stb_image          │    │                                      │
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
| `AnimationAssetData` | Animation clips with keyframe tracks |

## Enums

### AssetType

| Value | Description |
|-------|-------------|
| `Unknown` | Type not yet determined |
| `Mesh` | 3D model geometry |
| `Texture` | 2D texture image |
| `Material` | Material definition |
| `Audio` | Sound data |
| `Animation` | Animation clip |
| `Prefab` | Entity prefab template |
| `Scene` | Scene definition |
| `Shader` | Shader program |
| `Font` | Font asset |

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

## Supported Formats

### 3D Models

| Format | Description | Library | Loader Method |
|--------|-------------|---------|---------------|
| `.obj` | Wavefront OBJ | tinyobjloader | `LoadOBJ()` |
| `.fbx` | Autodesk FBX (meshes, skeletons, animations) | Assimp | `LoadFBX()` |
| `.gltf` / `.glb` | glTF 2.0 (PBR materials, animations) | Assimp | `LoadGLTF()` |

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
| `.wav` | Waveform audio | XAudio2 / miniaudio |

### Scenes and Data

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

Each vertex is **96 bytes** (3+3+3+2+2+4+4+4 floats/uints). The vertex layout supports:

- Static meshes (position, normal, tangent, UVs, color)
- Skeletal meshes (adds boneIndices and boneWeights for up to 4 bone influences)
- Lightmapped meshes (uses texCoord1 for lightmap UVs)

### AnimationAssetData

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

### FBX Files (Assimp)

Full-featured import for animated characters and complex scenes:

```cpp
auto mesh = pipeline.LoadMesh("Assets/Models/character.fbx");
// Imports: mesh geometry, materials, bone hierarchy,
//          skinning data, animation clips
```

### glTF 2.0 Files (Assimp)

Modern PBR workflow with embedded or referenced textures:

```cpp
auto mesh = pipeline.LoadMesh("Assets/Models/weapon.gltf");
// Imports: PBR materials (metallic/roughness),
//          mesh geometry, animations, morph targets
```

Assimp imports the following data from all model formats:

| Data | Description |
|------|-------------|
| Mesh geometry | Vertices, normals, UVs (2 channels), tangents, vertex colors |
| Materials | Mapped to PBR properties (albedo, normal, metallic, roughness) |
| Bone hierarchy | Skeletal structure for skinned meshes |
| Skinning data | Bone indices and weights (4 bones per vertex) |
| Animation clips | Keyframe tracks per bone (position, rotation, scale) |
| Bounding volumes | AABB and bounding sphere computed automatically |

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
5. **Use `.gltf`/`.glb`** for new content -- more efficient than FBX for PBR workflows
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
| FBX import missing bones | Assimp flags not set correctly | Verify Assimp import flags include `aiProcess_PopulateArmatureData` |
| glTF textures not loading | Texture paths relative to .gltf file | Ensure textures are in the expected relative path |

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
- [Animation](../subsystems/Animation.md) -- Importing animated models
- [Shader Pipeline](Shader-Pipeline.md) -- Shader compilation
- [Scene Management](../subsystems/Scene-Management.md) -- Scene file loading
- [Audio](../subsystems/Audio.md) -- Audio asset formats and loading
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation.md) -- Terrain asset streaming
- [Entity Component System](../subsystems/Entity-Component-System.md) -- Component-based asset references
