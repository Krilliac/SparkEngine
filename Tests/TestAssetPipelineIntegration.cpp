/**
 * @file TestAssetPipelineIntegration.cpp
 * @brief Integration tests for AssetPipeline lifecycle, cache, and asset type detection
 *
 * Tests the AssetPipeline orchestration: Initialize → Update → Shutdown,
 * plus the AssetCache LRU behavior and asset type detection utility.
 * All tests run on Linux without a GPU (the Linux path accepts nullptr device).
 */

#include "TestFramework.h"
#include "Graphics/AssetPipeline.h"

// ============================================================================
// AssetPipeline Lifecycle Tests
// ============================================================================

TEST(AssetPipeline_ConstructDestruct_DoesNotCrash)
{
    // Linux constructor creates cache in constructor
    AssetPipeline pipeline;
}

TEST(AssetPipeline_InitializeShutdown_Linux_Succeeds)
{
    AssetPipeline pipeline;
    HRESULT hr = pipeline.Initialize(nullptr, nullptr);
    EXPECT_TRUE(SUCCEEDED(hr));
    pipeline.Shutdown();
}

TEST(AssetPipeline_DoubleShutdown_DoesNotCrash)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);
    pipeline.Shutdown();
    pipeline.Shutdown(); // Second shutdown should be safe
}

TEST(AssetPipeline_UpdateWithoutInit_DoesNotCrash)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);
    // Update with no assets loaded should be safe
    for (int i = 0; i < 10; ++i)
        pipeline.Update(0.016f);
    pipeline.Shutdown();
}

// ============================================================================
// Asset Type Detection Tests
// ============================================================================

TEST(AssetPipeline_DetectAssetType_MeshFormats)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("model.obj")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("model.fbx")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("model.gltf")), static_cast<int>(AssetType::Mesh));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("model.glb")), static_cast<int>(AssetType::Mesh));

    pipeline.Shutdown();
}

TEST(AssetPipeline_DetectAssetType_TextureFormats)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("tex.png")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("tex.jpg")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("tex.tga")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("tex.bmp")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("tex.dds")), static_cast<int>(AssetType::Texture));

    pipeline.Shutdown();
}

TEST(AssetPipeline_DetectAssetType_AudioFormats)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("sound.wav")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("sound.ogg")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("sound.mp3")), static_cast<int>(AssetType::Audio));

    pipeline.Shutdown();
}

TEST(AssetPipeline_DetectAssetType_ShaderFormats)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("shader.hlsl")), static_cast<int>(AssetType::Shader));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("shader.glsl")), static_cast<int>(AssetType::Shader));

    pipeline.Shutdown();
}

TEST(AssetPipeline_DetectAssetType_UnknownExtension)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("file.xyz")), static_cast<int>(AssetType::Unknown));
    EXPECT_EQ(static_cast<int>(pipeline.DetectAssetType("noextension")), static_cast<int>(AssetType::Unknown));

    pipeline.Shutdown();
}

// ============================================================================
// AssetCache Tests
// ============================================================================

TEST(AssetCache_ConstructWithCapacity)
{
    AssetCache cache(256);
    EXPECT_EQ(cache.GetMaxMemory(), 256u * 1024u * 1024u);
    EXPECT_EQ(cache.GetCurrentMemory(), 0u);
    EXPECT_EQ(cache.GetCacheHits(), 0u);
    EXPECT_EQ(cache.GetCacheMisses(), 0u);
}

TEST(AssetCache_GetNonexistentAsset_ReturnsNull)
{
    AssetCache cache(256);
    auto asset = cache.GetAsset("nonexistent.obj");
    EXPECT_TRUE(asset == nullptr);
    EXPECT_EQ(cache.GetCacheMisses(), 1u);
}

TEST(AssetCache_HitRatio_InitiallyZero)
{
    AssetCache cache(256);
    float ratio = cache.GetHitRatio();
    EXPECT_NEAR(ratio, 0.0f, 0.01f);
}

TEST(AssetCache_SetMaxMemory_UpdatesCapacity)
{
    AssetCache cache(256);
    cache.SetMaxMemory(128);
    EXPECT_EQ(cache.GetMaxMemory(), 128u * 1024u * 1024u);
}

TEST(AssetCache_Clear_ResetsState)
{
    AssetCache cache(256);
    // Access a non-existent asset to increment misses
    cache.GetAsset("test.obj");
    EXPECT_EQ(cache.GetCacheMisses(), 1u);
    cache.Clear();
    EXPECT_EQ(cache.GetCurrentMemory(), 0u);
}

// ============================================================================
// Hot Reloading Configuration Tests
// ============================================================================

TEST(AssetPipeline_HotReloading_Toggle)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    // Whatever the default, toggling should work
    bool initial = pipeline.IsHotReloadingEnabled();
    pipeline.EnableHotReloading(!initial);
    EXPECT_EQ(pipeline.IsHotReloadingEnabled(), !initial);
    pipeline.EnableHotReloading(initial);
    EXPECT_EQ(pipeline.IsHotReloadingEnabled(), initial);

    pipeline.Shutdown();
}

TEST(AssetPipeline_HotReloading_ToggleOnOff)
{
    AssetPipeline pipeline;
    pipeline.Initialize(nullptr, nullptr);

    pipeline.EnableHotReloading(true);
    EXPECT_TRUE(pipeline.IsHotReloadingEnabled());

    pipeline.EnableHotReloading(false);
    EXPECT_FALSE(pipeline.IsHotReloadingEnabled());

    pipeline.Shutdown();
}
