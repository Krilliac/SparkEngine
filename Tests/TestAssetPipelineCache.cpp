/**
 * @file TestAssetPipelineCache.cpp
 * @brief Tests for AssetPipeline: LRU cache eviction, memory budget management,
 *        asset type detection, metadata tracking, and request queue ordering.
 *
 * Standalone reimplementation — mirrors Spark::AssetCache and AssetPipeline
 * without D3D11/GPU dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // --- Enums mirroring AssetPipeline.h ---

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

    enum class LoadingPriority
    {
        Low,
        Normal,
        High,
        Critical
    };

    enum class StreamingState
    {
        Unloaded,
        Loading,
        Loaded,
        Failed,
        Evicted
    };

    // --- Asset base ---

    class Asset
    {
      public:
        explicit Asset(const std::string& path, AssetType type, size_t memorySize)
            : m_path(path), m_type(type), m_memorySize(memorySize), m_state(StreamingState::Loaded)
        {
        }

        virtual ~Asset() = default;

        const std::string& GetPath() const { return m_path; }
        AssetType GetType() const { return m_type; }
        bool IsLoaded() const { return m_state == StreamingState::Loaded; }
        StreamingState GetState() const { return m_state; }
        size_t GetMemoryUsage() const { return m_memorySize; }
        void SetState(StreamingState state) { m_state = state; }

      private:
        std::string m_path;
        AssetType m_type;
        size_t m_memorySize;
        StreamingState m_state;
    };

    // --- AssetCache with LRU eviction ---

    class AssetCache
    {
      public:
        void SetMaxMemory(size_t maxMemoryBytes) { m_maxMemory = maxMemoryBytes; }
        size_t GetMaxMemory() const { return m_maxMemory; }
        size_t GetCurrentMemory() const { return m_currentMemory; }

        void AddAsset(std::shared_ptr<Asset> asset)
        {
            const auto& path = asset->GetPath();
            if (m_cache.count(path))
            {
                // Already exists — update access order
                TouchAsset(path);
                return;
            }

            m_cache[path] = asset;
            m_accessOrder.push_back(path);
            m_currentMemory += asset->GetMemoryUsage();
        }

        std::shared_ptr<Asset> GetAsset(const std::string& path)
        {
            auto it = m_cache.find(path);
            if (it != m_cache.end())
            {
                m_cacheHits++;
                TouchAsset(path);
                return it->second;
            }
            m_cacheMisses++;
            return nullptr;
        }

        void RemoveAsset(const std::string& path)
        {
            auto it = m_cache.find(path);
            if (it != m_cache.end())
            {
                m_currentMemory -= it->second->GetMemoryUsage();
                m_cache.erase(it);
                m_accessOrder.erase(std::remove(m_accessOrder.begin(), m_accessOrder.end(), path), m_accessOrder.end());
            }
        }

        void EvictLRU()
        {
            while (m_currentMemory > m_maxMemory && !m_accessOrder.empty())
            {
                const auto& lruPath = m_accessOrder.front();
                auto it = m_cache.find(lruPath);
                if (it != m_cache.end())
                {
                    it->second->SetState(StreamingState::Evicted);
                    m_currentMemory -= it->second->GetMemoryUsage();
                    m_cache.erase(it);
                }
                m_accessOrder.erase(m_accessOrder.begin());
            }
        }

        void Clear()
        {
            m_cache.clear();
            m_accessOrder.clear();
            m_currentMemory = 0;
        }

        uint32_t GetCacheHits() const { return m_cacheHits; }
        uint32_t GetCacheMisses() const { return m_cacheMisses; }
        float GetHitRatio() const
        {
            uint32_t total = m_cacheHits + m_cacheMisses;
            return total > 0 ? static_cast<float>(m_cacheHits) / static_cast<float>(total) : 0.0f;
        }
        size_t GetAssetCount() const { return m_cache.size(); }

      private:
        void TouchAsset(const std::string& path)
        {
            m_accessOrder.erase(std::remove(m_accessOrder.begin(), m_accessOrder.end(), path), m_accessOrder.end());
            m_accessOrder.push_back(path);
        }

        std::unordered_map<std::string, std::shared_ptr<Asset>> m_cache;
        std::vector<std::string> m_accessOrder; // front = LRU, back = MRU
        size_t m_maxMemory = 256 * 1024 * 1024; // 256 MB default
        size_t m_currentMemory = 0;
        uint32_t m_cacheHits = 0;
        uint32_t m_cacheMisses = 0;
    };

    // --- Asset type detection ---

    AssetType DetectAssetType(const std::string& path)
    {
        auto ext = path.substr(path.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == "obj" || ext == "fbx" || ext == "gltf" || ext == "glb")
            return AssetType::Mesh;
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "dds" || ext == "bmp")
            return AssetType::Texture;
        if (ext == "mat" || ext == "material")
            return AssetType::Material;
        if (ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac")
            return AssetType::Audio;
        if (ext == "anim" || ext == "animation")
            return AssetType::Animation;
        if (ext == "prefab")
            return AssetType::Prefab;
        if (ext == "scene" || ext == "level")
            return AssetType::Scene;
        if (ext == "hlsl" || ext == "glsl" || ext == "shader")
            return AssetType::Shader;
        if (ext == "ttf" || ext == "otf")
            return AssetType::Font;
        return AssetType::Unknown;
    }

    // --- Load request priority queue ---

    struct AssetLoadRequest
    {
        std::string assetPath;
        AssetType expectedType = AssetType::Unknown;
        LoadingPriority priority = LoadingPriority::Normal;

        bool operator<(const AssetLoadRequest& other) const
        {
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
    };

} // anonymous namespace

// ============================================================================
// LRU Cache Eviction Tests
// ============================================================================

TEST(AssetCache_AddAndRetrieve)
{
    AssetCache cache;
    auto asset = std::make_shared<Asset>("models/cube.obj", AssetType::Mesh, 1024);
    cache.AddAsset(asset);

    auto retrieved = cache.GetAsset("models/cube.obj");
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->GetPath(), std::string("models/cube.obj"));
}

TEST(AssetCache_MissReturnsNull)
{
    AssetCache cache;
    auto result = cache.GetAsset("nonexistent.obj");
    EXPECT_TRUE(result == nullptr);
}

TEST(AssetCache_EvictLRU_RemovesOldest)
{
    AssetCache cache;
    cache.SetMaxMemory(2000);

    auto a1 = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1000);
    auto a2 = std::make_shared<Asset>("a2.obj", AssetType::Mesh, 1000);
    auto a3 = std::make_shared<Asset>("a3.obj", AssetType::Mesh, 1000);

    cache.AddAsset(a1);
    cache.AddAsset(a2);
    cache.AddAsset(a3); // now at 3000, over budget

    cache.EvictLRU(); // should evict a1

    EXPECT_TRUE(cache.GetAsset("a1.obj") == nullptr);
    EXPECT_TRUE(cache.GetAsset("a2.obj") != nullptr);
    EXPECT_LE(cache.GetCurrentMemory(), cache.GetMaxMemory());
}

TEST(AssetCache_EvictLRU_PreservesMRU)
{
    AssetCache cache;
    cache.SetMaxMemory(2000);

    auto a1 = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1000);
    auto a2 = std::make_shared<Asset>("a2.obj", AssetType::Mesh, 1000);
    auto a3 = std::make_shared<Asset>("a3.obj", AssetType::Mesh, 1000);

    cache.AddAsset(a1);
    cache.AddAsset(a2);

    // Access a1 to make it MRU
    cache.GetAsset("a1.obj");

    cache.AddAsset(a3); // now over budget
    cache.EvictLRU();

    // a2 should be evicted (LRU), a1 preserved (accessed more recently)
    EXPECT_TRUE(cache.GetAsset("a2.obj") == nullptr);
    EXPECT_TRUE(cache.GetAsset("a1.obj") != nullptr);
}

TEST(AssetCache_EvictMultiple)
{
    AssetCache cache;
    cache.SetMaxMemory(1000);

    for (int i = 0; i < 10; i++)
    {
        auto asset = std::make_shared<Asset>("asset" + std::to_string(i) + ".obj", AssetType::Mesh, 500);
        cache.AddAsset(asset);
    }

    // 10 × 500 = 5000, budget is 1000
    cache.EvictLRU();
    EXPECT_LE(cache.GetCurrentMemory(), (size_t)1000);
    EXPECT_LE(cache.GetAssetCount(), (size_t)2);
}

TEST(AssetCache_EvictSetsState)
{
    AssetCache cache;
    cache.SetMaxMemory(500);

    auto a1 = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 400);
    auto a2 = std::make_shared<Asset>("a2.obj", AssetType::Mesh, 400);
    cache.AddAsset(a1);
    cache.AddAsset(a2);

    cache.EvictLRU();

    // a1 was evicted
    EXPECT_EQ(static_cast<int>(a1->GetState()), static_cast<int>(StreamingState::Evicted));
}

// ============================================================================
// Memory Budget Tests
// ============================================================================

TEST(AssetCache_MemoryTracking)
{
    AssetCache cache;
    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)0);

    auto a1 = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1024);
    cache.AddAsset(a1);
    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)1024);

    auto a2 = std::make_shared<Asset>("a2.obj", AssetType::Mesh, 2048);
    cache.AddAsset(a2);
    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)3072);
}

TEST(AssetCache_RemoveReducesMemory)
{
    AssetCache cache;
    auto asset = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1024);
    cache.AddAsset(asset);

    cache.RemoveAsset("a1.obj");
    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)0);
    EXPECT_EQ(cache.GetAssetCount(), (size_t)0);
}

TEST(AssetCache_ClearResetsEverything)
{
    AssetCache cache;
    cache.AddAsset(std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1024));
    cache.AddAsset(std::make_shared<Asset>("a2.obj", AssetType::Mesh, 2048));
    cache.Clear();

    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)0);
    EXPECT_EQ(cache.GetAssetCount(), (size_t)0);
}

TEST(AssetCache_DuplicateAddDoesNotDoubleCount)
{
    AssetCache cache;
    auto asset = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1024);
    cache.AddAsset(asset);
    cache.AddAsset(asset); // duplicate

    EXPECT_EQ(cache.GetCurrentMemory(), (size_t)1024);
    EXPECT_EQ(cache.GetAssetCount(), (size_t)1);
}

// ============================================================================
// Cache Hit/Miss Tracking
// ============================================================================

TEST(AssetCache_HitMissTracking)
{
    AssetCache cache;
    auto asset = std::make_shared<Asset>("a1.obj", AssetType::Mesh, 1024);
    cache.AddAsset(asset);

    cache.GetAsset("a1.obj");      // hit
    cache.GetAsset("a1.obj");      // hit
    cache.GetAsset("missing.obj"); // miss

    EXPECT_EQ(cache.GetCacheHits(), (uint32_t)2);
    EXPECT_EQ(cache.GetCacheMisses(), (uint32_t)1);
    EXPECT_NEAR(cache.GetHitRatio(), 2.0f / 3.0f, 0.01f);
}

TEST(AssetCache_HitRatioZeroWhenEmpty)
{
    AssetCache cache;
    EXPECT_NEAR(cache.GetHitRatio(), 0.0f, 0.001f);
}

// ============================================================================
// Asset Type Detection Tests
// ============================================================================

TEST(AssetPipeline_DetectMeshTypes)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("model.obj")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(DetectAssetType("model.fbx")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(DetectAssetType("model.gltf")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(DetectAssetType("model.glb")), static_cast<int>(AssetType::Mesh));
}

TEST(AssetPipeline_DetectTextureTypes)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("tex.png")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectAssetType("tex.jpg")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectAssetType("tex.dds")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectAssetType("tex.tga")), static_cast<int>(AssetType::Texture));
}

TEST(AssetPipeline_DetectAudioTypes)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("sound.wav")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(DetectAssetType("music.ogg")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(DetectAssetType("voice.mp3")), static_cast<int>(AssetType::Audio));
}

TEST(AssetPipeline_DetectShaderTypes)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("vert.hlsl")), static_cast<int>(AssetType::Shader));
    EXPECT_EQ(static_cast<int>(DetectAssetType("frag.glsl")), static_cast<int>(AssetType::Shader));
}

TEST(AssetPipeline_DetectFontTypes)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("arial.ttf")), static_cast<int>(AssetType::Font));
    EXPECT_EQ(static_cast<int>(DetectAssetType("custom.otf")), static_cast<int>(AssetType::Font));
}

TEST(AssetPipeline_DetectUnknown)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("readme.txt")), static_cast<int>(AssetType::Unknown));
    EXPECT_EQ(static_cast<int>(DetectAssetType("data.bin")), static_cast<int>(AssetType::Unknown));
}

TEST(AssetPipeline_DetectCaseInsensitive)
{
    EXPECT_EQ(static_cast<int>(DetectAssetType("model.FBX")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(DetectAssetType("tex.PNG")), static_cast<int>(AssetType::Texture));
}

// ============================================================================
// Request Queue Priority Tests
// ============================================================================

TEST(AssetPipeline_RequestPriorityOrdering)
{
    std::priority_queue<AssetLoadRequest> queue;

    AssetLoadRequest low{"low.obj", AssetType::Mesh, LoadingPriority::Low};
    AssetLoadRequest normal{"normal.obj", AssetType::Mesh, LoadingPriority::Normal};
    AssetLoadRequest high{"high.obj", AssetType::Mesh, LoadingPriority::High};
    AssetLoadRequest critical{"critical.obj", AssetType::Mesh, LoadingPriority::Critical};

    queue.push(low);
    queue.push(normal);
    queue.push(critical);
    queue.push(high);

    // Critical should come out first
    EXPECT_EQ(queue.top().assetPath, std::string("critical.obj"));
    queue.pop();
    EXPECT_EQ(queue.top().assetPath, std::string("high.obj"));
    queue.pop();
    EXPECT_EQ(queue.top().assetPath, std::string("normal.obj"));
    queue.pop();
    EXPECT_EQ(queue.top().assetPath, std::string("low.obj"));
}

// ============================================================================
// Max Memory Configuration Tests
// ============================================================================

TEST(AssetCache_SetMaxMemory)
{
    AssetCache cache;
    cache.SetMaxMemory(512 * 1024 * 1024);
    EXPECT_EQ(cache.GetMaxMemory(), (size_t)(512 * 1024 * 1024));
}

TEST(AssetCache_DefaultMaxMemory)
{
    AssetCache cache;
    EXPECT_EQ(cache.GetMaxMemory(), (size_t)(256 * 1024 * 1024));
}
