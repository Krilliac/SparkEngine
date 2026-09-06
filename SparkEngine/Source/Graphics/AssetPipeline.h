/**
 * @file AssetPipeline.h
 * @brief Complete asset pipeline system for model loading and streaming
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive asset management system supporting
 * model loading, texture streaming, asset cooking, and runtime optimization.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "Utils/Hash.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <future>

using Microsoft::WRL::ComPtr;

// Forward declarations — MeshAsset (and the non-Windows BindMesh / ProcessDrawList
// path) needs to hold RHI vertex/index buffers, and TextureAsset holds an
// RHI texture, but pulling in the full RHIResources.h would drag the
// transitive RHI headers into every TU that touches AssetPipeline. The .cpp
// files that actually construct the resources include RHIResources.h /
// RHI.h directly.
namespace Spark::RHI
{
    class IRHIBuffer;
    class IRHITexture;
} // namespace Spark::RHI

/**
 * @brief Asset types supported by the pipeline
 */
enum class AssetType
{
    Unknown,
    Mesh,
    Texture,
    Material,
    Audio,
    Animation,
    Prefab,
    Scene,
    Shader,
    Font
};

/**
 * @brief Asset loading priority
 */
enum class LoadingPriority
{
    Low,
    Normal,
    High,
    Critical
};

/**
 * @brief Asset streaming state
 */
enum class StreamingState
{
    Unloaded,
    Loading,
    Loaded,
    Failed,
    Evicted
};

/**
 * @brief Mesh data structure
 */
struct MeshAssetData
{
    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT3 tangent;
        XMFLOAT2 texCoord0;
        XMFLOAT2 texCoord1;
        XMFLOAT4 color;
        XMUINT4 boneIndices;
        XMFLOAT4 boneWeights;
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> submeshes; // Submesh start indices
    XMFLOAT3 boundingBoxMin;
    XMFLOAT3 boundingBoxMax;
    float boundingSphereRadius;
    XMFLOAT3 boundingSphereCenter;
};

/**
 * @brief Animation data structure
 */
struct AnimationAssetData
{
    struct Keyframe
    {
        float time;
        XMFLOAT3 position;
        XMFLOAT4 rotation;
        XMFLOAT3 scale;
    };

    struct AnimationTrack
    {
        std::string boneName;
        std::vector<Keyframe> keyframes;
    };

    std::string name;
    float duration;
    float ticksPerSecond;
    std::vector<AnimationTrack> tracks;
};

/**
 * @brief Asset metadata
 */
struct AssetMetadata
{
    std::string guid;                                              ///< Unique asset identifier
    std::string filePath;                                          ///< Original file path
    std::string name;                                              ///< Asset name
    AssetType type;                                                ///< Asset type
    size_t fileSize;                                               ///< File size in bytes
    size_t memorySize;                                             ///< Memory footprint
    uint64_t lastModified;                                         ///< Last modification timestamp
    std::string checksum;                                          ///< Content checksum
    std::vector<std::string> dependencies;                         ///< Asset dependencies
    LoadingPriority priority;                                      ///< Loading priority
    StreamingState state;                                          ///< Current streaming state
    std::unordered_map<std::string, std::string> customProperties; ///< Custom metadata
};

/**
 * @brief Asset loading request
 */
struct AssetLoadRequest
{
    std::string assetPath;
    AssetType expectedType;
    LoadingPriority priority;
    std::function<void(std::shared_ptr<void>)> onLoaded;
    std::function<void(const std::string&)> onError;
    bool blocking = false;
};

/**
 * @brief Asset base class
 */
class Asset
{
  public:
    Asset(const std::string& path, AssetType type) : m_path(path), m_type(type), m_loaded(false) {}
    virtual ~Asset() = default;

    const std::string& GetPath() const { return m_path; }
    AssetType GetType() const { return m_type; }
    bool IsLoaded() const { return m_loaded; }
    const AssetMetadata& GetMetadata() const { return m_metadata; }

    virtual HRESULT Load(ID3D11Device* device) = 0;
    virtual void Unload() = 0;
    virtual size_t GetMemoryUsage() const = 0;

  protected:
    std::string m_path;
    AssetType m_type;
    bool m_loaded;
    AssetMetadata m_metadata;
};

/**
 * @brief Mesh asset
 */
class MeshAsset : public Asset
{
  public:
    // Out-of-line ctor / dtor so the `std::unique_ptr<Spark::RHI::IRHIBuffer>`
    // members can work with the forward-declared type above. Their
    // definitions live in AssetTypes.cpp where `RHIResources.h` is
    // included and `IRHIBuffer` is complete — the inline versions would
    // require every TU including this header to see the full RHI headers.
    explicit MeshAsset(const std::string& path);
    ~MeshAsset() override;

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    const MeshAssetData& GetMeshData() const { return m_meshData; }
    ID3D11Buffer* GetVertexBuffer() const { return m_vertexBuffer.Get(); }
    ID3D11Buffer* GetIndexBuffer() const { return m_indexBuffer.Get(); }
    uint32_t GetVertexCount() const { return static_cast<uint32_t>(m_meshData.vertices.size()); }
    uint32_t GetIndexCount() const { return static_cast<uint32_t>(m_meshData.indices.size()); }

    // ------------------------------------------------------------------------
    // RHI buffer accessors — populated on Linux / macOS only. Windows keeps
    // driving the D3D11 ComPtr buffers above and leaves these as nullptr.
    // ------------------------------------------------------------------------
    Spark::RHI::IRHIBuffer* GetRHIVertexBuffer() const { return m_rhiVertexBuffer.get(); }
    Spark::RHI::IRHIBuffer* GetRHIIndexBuffer() const { return m_rhiIndexBuffer.get(); }

    /// Transfer ownership of pre-built RHI vertex/index buffers. Used by the
    /// non-Windows AssetPipeline::LoadMesh post-load hook that uploads the
    /// CPU-side `m_meshData` into the bridge's device. Replaces any previous
    /// buffers (safe across hot-reload).
    void SetRHIBuffers(std::unique_ptr<Spark::RHI::IRHIBuffer> vb, std::unique_ptr<Spark::RHI::IRHIBuffer> ib);

  private:
    MeshAssetData m_meshData;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11Buffer> m_indexBuffer;

    // RHI-backed buffers for the non-Windows render path. Declared after the
    // D3D11 ComPtrs so the destructor order teardown is: RHI bufs → ComPtrs →
    // mesh data vectors (matches "last constructed, first destroyed" with no
    // cross-member references either direction).
    std::unique_ptr<Spark::RHI::IRHIBuffer> m_rhiVertexBuffer;
    std::unique_ptr<Spark::RHI::IRHIBuffer> m_rhiIndexBuffer;
};

/**
 * @brief Texture asset
 */
class TextureAsset : public Asset
{
  public:
    // Out-of-line ctor / dtor for the same reason as MeshAsset: the
    // `std::unique_ptr<Spark::RHI::IRHITexture>` member needs the type
    // complete at destruction time, and we want RHIResources.h confined
    // to the .cpp. Definitions live in AssetTypes.cpp.
    explicit TextureAsset(const std::string& path);
    ~TextureAsset() override;

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    // RHI texture handle — populated on Linux/macOS after image load, nullptr
    // on Windows (which uses the D3D11 ComPtrs above). Non-owning pointer
    // accessor for the render path; ownership stays with the unique_ptr.
    Spark::RHI::IRHITexture* GetRHITexture() const { return m_rhiTexture.get(); }
    void SetRHITexture(std::unique_ptr<Spark::RHI::IRHITexture> tex);

  private:
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // RHI-backed texture for the non-Windows render path. Declared after
    // the D3D11 ComPtrs so destruction order is predictable.
    std::unique_ptr<Spark::RHI::IRHITexture> m_rhiTexture;
};

/**
 * @brief Audio asset
 */
class AudioAsset : public Asset
{
  public:
    AudioAsset(const std::string& path) : Asset(path, AssetType::Audio) {}

    HRESULT Load(ID3D11Device* device) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    const std::vector<uint8_t>& GetAudioData() const { return m_audioData; }
    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint32_t GetChannels() const { return m_channels; }
    uint32_t GetBitsPerSample() const { return m_bitsPerSample; }

  private:
    std::vector<uint8_t> m_audioData;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bitsPerSample = 0;
};

/**
 * @brief Asset cache with LRU eviction
 */
class AssetCache
{
  public:
    AssetCache(size_t maxMemoryMB = 512);
    ~AssetCache();

    void SetMaxMemory(size_t maxMemoryMB);
    size_t GetMaxMemory() const { return m_maxMemory; }
    size_t GetCurrentMemory() const;

    void AddAsset(std::shared_ptr<Asset> asset);
    std::shared_ptr<Asset> GetAsset(const std::string& path);
    void RemoveAsset(const std::string& path);
    void EvictLRU();
    void Clear();

    // Statistics
    uint32_t GetCacheHits() const { return m_hits; }
    uint32_t GetCacheMisses() const { return m_misses; }
    float GetHitRatio() const;

  private:
    struct CacheEntry
    {
        std::shared_ptr<Asset> asset;
        uint64_t lastAccessed;
        size_t accessCount;
    };

    /// Sum of GetMemoryUsage() over the cached entries. Caller holds m_mutex.
    size_t CurrentMemoryLocked() const;
    /// Drop the least recently accessed entry. Caller holds m_mutex.
    void EvictLRULocked();

    // Non-recursive on purpose. AddAsset() used to call the public
    // GetCurrentMemory()/EvictLRU() while already holding this lock; MSVC's STL
    // reports that re-lock as system_error(resource_deadlock_would_occur), so
    // every synchronous AssetPipeline::LoadAsset() that reached the cache
    // threw. Internal callers use the *Locked helpers above instead.
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CacheEntry> m_cache;
    size_t m_maxMemory;
    uint32_t m_hits = 0;
    uint32_t m_misses = 0;
};

/**
 * @brief Asset pipeline system
 */
class AssetPipeline
{
  public:
    /**
     * @brief Asset pipeline metrics
     */
    struct AssetMetrics
    {
        uint32_t totalAssets;      ///< Total number of assets
        uint32_t loadedAssets;     ///< Currently loaded assets
        uint32_t pendingRequests;  ///< Pending load requests
        uint32_t failedLoads;      ///< Failed load attempts
        size_t memoryUsage;        ///< Current memory usage (bytes)
        size_t maxMemoryUsage;     ///< Maximum memory usage (bytes)
        float averageLoadTime;     ///< Average asset load time (ms)
        float cacheHitRatio;       ///< Cache hit ratio (0-1)
        uint32_t streamingThreads; ///< Number of streaming threads
        bool backgroundLoading;    ///< Background loading enabled
    };

    AssetPipeline();
    ~AssetPipeline();

    /**
     * @brief Initialize the asset pipeline
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the asset pipeline
     */
    void Shutdown();

    /**
     * @brief Update the asset pipeline (call per frame)
     */
    void Update(float deltaTime);

    // Synchronous loading
    std::shared_ptr<Asset> LoadAsset(const std::string& path, AssetType type = AssetType::Unknown);
    std::shared_ptr<MeshAsset> LoadMesh(const std::string& path);
    std::shared_ptr<TextureAsset> LoadTexture(const std::string& path);
    std::shared_ptr<AudioAsset> LoadAudio(const std::string& path);

    // Asynchronous loading
    void LoadAssetAsync(const AssetLoadRequest& request);
    void LoadMeshAsync(const std::string& path, std::function<void(std::shared_ptr<MeshAsset>)> callback);
    void LoadTextureAsync(const std::string& path, std::function<void(std::shared_ptr<TextureAsset>)> callback);

    // Asset management
    void UnloadAsset(const std::string& path);
    void UnloadAllAssets();
    std::shared_ptr<Asset> GetAsset(const std::string& path);
    bool IsAssetLoaded(const std::string& path) const;

    // Cache management
    void SetCacheSize(size_t maxMemoryMB);
    void EvictUnusedAssets();
    void PreloadAssets(const std::vector<std::string>& paths);

    // Streaming
    void EnableBackgroundStreaming(bool enabled);
    bool IsBackgroundStreamingEnabled() const { return m_backgroundStreaming; }
    void SetStreamingThreadCount(int count);
    int GetStreamingThreadCount() const { return static_cast<int>(m_loadingThreads.size()); }

    // Rendering helpers (bind asset for drawing via graphics context)
    // string_view overloads are hot-path friendly: no temporary std::string is
    // constructed for lookup when the map uses transparent hashing (see m_assets).
    /// Bind the mesh at @p meshPath for drawing.
    /// @return true when the GPU vertex/index buffers were actually bound. On
    ///         false the bound-mesh pointer is cleared, so a following
    ///         DrawBoundMesh() draws nothing instead of rasterizing whatever was
    ///         bound before. Callers that count draws must check this.
    bool BindMesh(std::string_view meshPath);
    void BindMaterial(std::string_view materialPath);
    /// Draw the mesh recorded by the last successful BindMesh(). No-op when the
    /// last bind failed or nothing was ever bound.
    void DrawBoundMesh();

    // Non-Windows: uploads the freshly-loaded CPU-side mesh data
    // (`mesh.GetMeshData()`) into RHI vertex/index buffers via the
    // Linux/macOS RHI bridge singleton, and hands ownership to the MeshAsset
    // via `SetRHIBuffers`. No-op when the bridge isn't initialized yet,
    // when the mesh has no vertex data, or when buffer creation fails —
    // the render path degrades cleanly in all cases (guarded on
    // `GetRHIVertexBuffer() != nullptr`).
    //
    // Windows routes buffer uploads through `ID3D11Device` inside
    // `MeshAsset::Load` directly, so this is a no-op there. Public so
    // tests and late-loading code paths can call it explicitly; typical
    // production usage is the post-load hook in `LoadMesh`.
    void BuildRHIBuffersForMesh(MeshAsset& mesh);

    // Asset discovery
    std::vector<std::string> ScanDirectory(const std::string& directory, AssetType type = AssetType::Unknown);
    AssetType DetectAssetType(const std::string& path) const;

    // Metadata
    AssetMetadata GetAssetMetadata(const std::string& path) const;
    void RefreshAssetMetadata(const std::string& path);

    // Hot reloading
    void EnableHotReloading(bool enabled) { m_hotReloadingEnabled = enabled; }
    bool IsHotReloadingEnabled() const { return m_hotReloadingEnabled; }
    void CheckForChangedAssets();

    // Metrics
    AssetMetrics GetMetrics() const;

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Get asset pipeline metrics
     */
    AssetMetrics Console_GetMetrics() const;

    /**
     * @brief List all loaded assets
     */
    std::string Console_ListAssets() const;

    /**
     * @brief Get asset information
     */
    std::string Console_GetAssetInfo(const std::string& path) const;

    /**
     * @brief Load asset via console
     */
    bool Console_LoadAsset(const std::string& path);

    /**
     * @brief Unload asset via console
     */
    bool Console_UnloadAsset(const std::string& path);

    /**
     * @brief Set cache size
     */
    void Console_SetCacheSize(size_t maxMemoryMB);

    /**
     * @brief Force garbage collection
     */
    void Console_ForceGC();

    /**
     * @brief Enable/disable background streaming
     */
    void Console_EnableStreaming(bool enabled);

    /**
     * @brief Set streaming thread count
     */
    void Console_SetStreamingThreads(int count);

    /**
     * @brief Scan directory for assets
     */
    int Console_ScanDirectory(const std::string& directory);

    /**
     * @brief Enable/disable hot reloading
     */
    void Console_EnableHotReload(bool enabled);

    /**
     * @brief Preload assets from directory
     */
    int Console_PreloadDirectory(const std::string& directory);

    /**
     * @brief Reload all assets
     */
    int Console_ReloadAllAssets();

  private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;

    // Asset storage
    // Transparent hashing enables lookup by std::string_view / const char*
    // without constructing a temporary std::string — called per-draw-call by
    // BindMesh / BindMaterial on the render hot path.
    mutable std::mutex m_assetsMutex;
    std::unordered_map<std::string, std::shared_ptr<Asset>, Spark::TransparentStringHash, Spark::TransparentStringEqual>
        m_assets;
    std::unique_ptr<AssetCache> m_cache;

    // Loading system
    bool m_backgroundStreaming = true;
    std::vector<std::thread> m_loadingThreads;
    std::queue<AssetLoadRequest> m_loadQueue;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::atomic<bool> m_shouldStop{false};

    // Hot reloading
    bool m_hotReloadingEnabled = true;
    std::unordered_map<std::string, uint64_t> m_fileTimestamps;

    // Metrics
    mutable std::mutex m_metricsMutex;
    AssetMetrics m_metrics;

    // Currently-bound mesh/texture for the non-Windows RHI draw path. Set by
    // BindMesh / BindMaterial, read by DrawBoundMesh. Non-owning — the
    // owning shared_ptr lives in `m_assets`. Windows BindMesh drives state
    // directly on the D3D11 context and doesn't need these to survive past
    // the call site (they still get written on Windows for symmetry but go
    // unused).
    MeshAsset* m_boundMeshAsset = nullptr;
    TextureAsset* m_boundTextureAsset = nullptr;

    // Helper methods
    void LoadingThreadFunction();
    AssetType DetectAssetTypeFromExtension(const std::string& extension) const;
    std::string CalculateChecksum(const std::string& filePath) const;
    uint64_t GetFileTimestamp(const std::string& filePath) const;
    void UpdateMetrics();

    // Asset loaders
    std::shared_ptr<MeshAsset> LoadMeshFromFile(const std::string& path);
    std::shared_ptr<TextureAsset> LoadTextureFromFile(const std::string& path);
    std::shared_ptr<AudioAsset> LoadAudioFromFile(const std::string& path);

    // Format-specific loaders
    HRESULT LoadOBJ(const std::string& path, MeshAssetData& meshData);
    HRESULT LoadFBX(const std::string& path, MeshAssetData& meshData);
    HRESULT LoadGLTF(const std::string& path, MeshAssetData& meshData);
};

// Utility functions
std::string AssetTypeToString(AssetType type);
AssetType StringToAssetType(const std::string& str);
std::string StreamingStateToString(StreamingState state);
std::string LoadingPriorityToString(LoadingPriority priority);
