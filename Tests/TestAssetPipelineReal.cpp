/**
 * @file TestAssetPipelineReal.cpp
 * @brief Real-class tests for AssetPipeline types and AssetCache
 *
 * Tests the actual production AssetPipeline.h — enums, metadata structs,
 * string conversion utilities, and AssetCache LRU behavior.
 */

#include "TestFramework.h"
#include "Graphics/AssetPipeline.h"

// ---------------------------------------------------------------------------
// AssetType enum
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AssetTypeEnumValues)
{
    EXPECT_EQ(static_cast<int>(AssetType::Unknown), 0);
    EXPECT_NE(static_cast<int>(AssetType::Mesh), static_cast<int>(AssetType::Texture));
    EXPECT_NE(static_cast<int>(AssetType::Material), static_cast<int>(AssetType::Audio));
    EXPECT_NE(static_cast<int>(AssetType::Animation), static_cast<int>(AssetType::Prefab));
    EXPECT_NE(static_cast<int>(AssetType::Scene), static_cast<int>(AssetType::Shader));
    EXPECT_NE(static_cast<int>(AssetType::Shader), static_cast<int>(AssetType::Font));
}

// ---------------------------------------------------------------------------
// LoadingPriority enum
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_LoadingPriorityOrder)
{
    EXPECT_LT(static_cast<int>(LoadingPriority::Low), static_cast<int>(LoadingPriority::Normal));
    EXPECT_LT(static_cast<int>(LoadingPriority::Normal), static_cast<int>(LoadingPriority::High));
    EXPECT_LT(static_cast<int>(LoadingPriority::High), static_cast<int>(LoadingPriority::Critical));
}

// ---------------------------------------------------------------------------
// StreamingState enum
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_StreamingStateValues)
{
    EXPECT_NE(static_cast<int>(StreamingState::Unloaded), static_cast<int>(StreamingState::Loaded));
    EXPECT_NE(static_cast<int>(StreamingState::Loading), static_cast<int>(StreamingState::Failed));
    EXPECT_NE(static_cast<int>(StreamingState::Loaded), static_cast<int>(StreamingState::Evicted));
}

// ---------------------------------------------------------------------------
// String conversion utilities
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AssetTypeToString)
{
    EXPECT_EQ(AssetTypeToString(AssetType::Mesh), std::string("Mesh"));
    EXPECT_EQ(AssetTypeToString(AssetType::Texture), std::string("Texture"));
    EXPECT_EQ(AssetTypeToString(AssetType::Material), std::string("Material"));
    EXPECT_EQ(AssetTypeToString(AssetType::Audio), std::string("Audio"));
    EXPECT_EQ(AssetTypeToString(AssetType::Animation), std::string("Animation"));
    EXPECT_EQ(AssetTypeToString(AssetType::Prefab), std::string("Prefab"));
    EXPECT_EQ(AssetTypeToString(AssetType::Scene), std::string("Scene"));
    EXPECT_EQ(AssetTypeToString(AssetType::Shader), std::string("Shader"));
    EXPECT_EQ(AssetTypeToString(AssetType::Font), std::string("Font"));
    EXPECT_EQ(AssetTypeToString(AssetType::Unknown), std::string("Unknown"));
}

TEST(AssetPipelineReal_StringToAssetType)
{
    EXPECT_TRUE(StringToAssetType("Mesh") == AssetType::Mesh);
    EXPECT_TRUE(StringToAssetType("Texture") == AssetType::Texture);
    EXPECT_TRUE(StringToAssetType("Audio") == AssetType::Audio);
    EXPECT_TRUE(StringToAssetType("Unknown") == AssetType::Unknown);
    EXPECT_TRUE(StringToAssetType("garbage") == AssetType::Unknown);
}

TEST(AssetPipelineReal_StreamingStateToString)
{
    EXPECT_EQ(StreamingStateToString(StreamingState::Unloaded), std::string("Unloaded"));
    EXPECT_EQ(StreamingStateToString(StreamingState::Loading), std::string("Loading"));
    EXPECT_EQ(StreamingStateToString(StreamingState::Loaded), std::string("Loaded"));
    EXPECT_EQ(StreamingStateToString(StreamingState::Failed), std::string("Failed"));
    EXPECT_EQ(StreamingStateToString(StreamingState::Evicted), std::string("Evicted"));
}

TEST(AssetPipelineReal_LoadingPriorityToString)
{
    EXPECT_EQ(LoadingPriorityToString(LoadingPriority::Low), std::string("Low"));
    EXPECT_EQ(LoadingPriorityToString(LoadingPriority::Normal), std::string("Normal"));
    EXPECT_EQ(LoadingPriorityToString(LoadingPriority::High), std::string("High"));
    EXPECT_EQ(LoadingPriorityToString(LoadingPriority::Critical), std::string("Critical"));
}

// ---------------------------------------------------------------------------
// AssetMetadata struct
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AssetMetadataDefaults)
{
    AssetMetadata meta;
    meta.guid = "test-guid";
    meta.filePath = "/path/to/asset.obj";
    meta.name = "TestAsset";
    meta.type = AssetType::Mesh;
    meta.fileSize = 1024;
    meta.memorySize = 2048;
    meta.lastModified = 0;
    meta.priority = LoadingPriority::Normal;
    meta.state = StreamingState::Unloaded;

    EXPECT_EQ(meta.guid, std::string("test-guid"));
    EXPECT_TRUE(meta.type == AssetType::Mesh);
    EXPECT_EQ(meta.fileSize, size_t(1024));
    EXPECT_EQ(meta.memorySize, size_t(2048));
    EXPECT_TRUE(meta.dependencies.empty());
    EXPECT_TRUE(meta.customProperties.empty());
}

// ---------------------------------------------------------------------------
// MeshAssetData struct
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_MeshAssetDataConstruction)
{
    MeshAssetData meshData;
    EXPECT_TRUE(meshData.vertices.empty());
    EXPECT_TRUE(meshData.indices.empty());
    EXPECT_TRUE(meshData.submeshes.empty());
}

TEST(AssetPipelineReal_MeshVertexFields)
{
    MeshAssetData::Vertex v{};
    v.position = {1.0f, 2.0f, 3.0f};
    v.normal = {0.0f, 1.0f, 0.0f};
    v.texCoord0 = {0.5f, 0.5f};

    EXPECT_NEAR(v.position.x, 1.0f, 0.001f);
    EXPECT_NEAR(v.normal.y, 1.0f, 0.001f);
    EXPECT_NEAR(v.texCoord0.x, 0.5f, 0.001f);
}

// ---------------------------------------------------------------------------
// AnimationAssetData struct
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AnimationAssetDataConstruction)
{
    AnimationAssetData anim;
    anim.name = "walk";
    anim.duration = 1.5f;
    anim.ticksPerSecond = 30.0f;
    EXPECT_TRUE(anim.tracks.empty());
    EXPECT_NEAR(anim.duration, 1.5f, 0.001f);
}

TEST(AssetPipelineReal_AnimationKeyframeFields)
{
    AnimationAssetData::Keyframe kf;
    kf.time = 0.5f;
    kf.position = {1.0f, 0.0f, 0.0f};
    kf.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    kf.scale = {1.0f, 1.0f, 1.0f};

    EXPECT_NEAR(kf.time, 0.5f, 0.001f);
    EXPECT_NEAR(kf.rotation.w, 1.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// AssetLoadRequest struct
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AssetLoadRequestDefaults)
{
    AssetLoadRequest req;
    req.assetPath = "models/test.obj";
    req.expectedType = AssetType::Mesh;
    req.priority = LoadingPriority::High;
    EXPECT_FALSE(req.blocking);
    EXPECT_EQ(req.assetPath, std::string("models/test.obj"));
}

// ---------------------------------------------------------------------------
// AssetPipeline::AssetMetrics
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_MetricsZeroInit)
{
    AssetPipeline::AssetMetrics metrics{};
    EXPECT_EQ(metrics.totalAssets, uint32_t(0));
    EXPECT_EQ(metrics.loadedAssets, uint32_t(0));
    EXPECT_EQ(metrics.pendingRequests, uint32_t(0));
    EXPECT_EQ(metrics.failedLoads, uint32_t(0));
    EXPECT_EQ(metrics.memoryUsage, size_t(0));
}

// ---------------------------------------------------------------------------
// AssetCache (constructor + basic API)
// ---------------------------------------------------------------------------

TEST(AssetPipelineReal_AssetCacheConstruction)
{
    AssetCache cache(256);
    EXPECT_EQ(cache.GetMaxMemory(), size_t(256 * 1024 * 1024));
    EXPECT_EQ(cache.GetCacheHits(), uint32_t(0));
    EXPECT_EQ(cache.GetCacheMisses(), uint32_t(0));
}

TEST(AssetPipelineReal_AssetCacheMissOnEmpty)
{
    AssetCache cache(64);
    auto asset = cache.GetAsset("nonexistent");
    EXPECT_TRUE(asset == nullptr);
    EXPECT_EQ(cache.GetCacheMisses(), uint32_t(1));
}

TEST(AssetPipelineReal_AssetCacheClear)
{
    AssetCache cache(64);
    cache.Clear();
    EXPECT_EQ(cache.GetCurrentMemory(), size_t(0));
}
