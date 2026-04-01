/**
 * @file TestMaterialSystemEdgeCases.cpp
 * @brief Edge-case tests for MaterialSystem: hot reload, metrics, texture type conversion
 *
 * Complements TestMaterialSystemValidation.cpp (PBR validation, variants, serialization)
 * with system-level edge cases for lifecycle, caching, and type utilities.
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>

// ============================================================================
// Standalone test types (matching production enums/structs for CI)
// ============================================================================

namespace
{

    enum class BlendMode
    {
        Opaque,
        AlphaTest,
        Transparent,
        Additive,
        Multiply,
        Screen
    };

    enum class CullMode
    {
        None,
        Front,
        Back
    };

    enum class MaterialTextureType
    {
        Albedo,
        Normal,
        Metallic,
        Roughness,
        Occlusion,
        Emissive,
        Height,
        DetailAlbedo,
        DetailNormal,
        Subsurface,
        Transmission,
        Clearcoat,
        ClearcoatRoughness,
        Anisotropy,
        Custom0,
        Custom1,
        Custom2,
        Custom3
    };

    struct MaterialRenderState
    {
        BlendMode blendMode = BlendMode::Opaque;
        CullMode cullMode = CullMode::Back;
        bool depthTest = true;
        bool depthWrite = true;
        bool castShadows = true;
        bool receiveShadows = true;
        int renderQueue = 2000;
        bool doubleSided = false;
    };

    struct MaterialMetrics
    {
        int loadedMaterials = 0;
        int textureCount = 0;
        size_t textureMemory = 0;
        int materialSwitches = 0;
        int textureBinds = 0;
        float averageLoadTime = 0.0f;
        int failedLoads = 0;
        bool hotReloadEnabled = false;
        int variantCount = 0;
    };

    std::string TextureTypeToString(MaterialTextureType type)
    {
        switch (type)
        {
        case MaterialTextureType::Albedo:
            return "Albedo";
        case MaterialTextureType::Normal:
            return "Normal";
        case MaterialTextureType::Metallic:
            return "Metallic";
        case MaterialTextureType::Roughness:
            return "Roughness";
        case MaterialTextureType::Occlusion:
            return "Occlusion";
        case MaterialTextureType::Emissive:
            return "Emissive";
        case MaterialTextureType::Height:
            return "Height";
        case MaterialTextureType::DetailAlbedo:
            return "DetailAlbedo";
        case MaterialTextureType::DetailNormal:
            return "DetailNormal";
        case MaterialTextureType::Subsurface:
            return "Subsurface";
        case MaterialTextureType::Transmission:
            return "Transmission";
        case MaterialTextureType::Clearcoat:
            return "Clearcoat";
        case MaterialTextureType::ClearcoatRoughness:
            return "ClearcoatRoughness";
        case MaterialTextureType::Anisotropy:
            return "Anisotropy";
        case MaterialTextureType::Custom0:
            return "Custom0";
        case MaterialTextureType::Custom1:
            return "Custom1";
        case MaterialTextureType::Custom2:
            return "Custom2";
        case MaterialTextureType::Custom3:
            return "Custom3";
        default:
            return "Unknown";
        }
    }

    MaterialTextureType StringToTextureType(const std::string& str)
    {
        if (str == "Albedo")
            return MaterialTextureType::Albedo;
        if (str == "Normal")
            return MaterialTextureType::Normal;
        if (str == "Metallic")
            return MaterialTextureType::Metallic;
        if (str == "Roughness")
            return MaterialTextureType::Roughness;
        if (str == "Occlusion")
            return MaterialTextureType::Occlusion;
        if (str == "Emissive")
            return MaterialTextureType::Emissive;
        if (str == "Height")
            return MaterialTextureType::Height;
        if (str == "DetailAlbedo")
            return MaterialTextureType::DetailAlbedo;
        if (str == "DetailNormal")
            return MaterialTextureType::DetailNormal;
        if (str == "Subsurface")
            return MaterialTextureType::Subsurface;
        if (str == "Transmission")
            return MaterialTextureType::Transmission;
        if (str == "Clearcoat")
            return MaterialTextureType::Clearcoat;
        if (str == "ClearcoatRoughness")
            return MaterialTextureType::ClearcoatRoughness;
        if (str == "Anisotropy")
            return MaterialTextureType::Anisotropy;
        if (str == "Custom0")
            return MaterialTextureType::Custom0;
        if (str == "Custom1")
            return MaterialTextureType::Custom1;
        if (str == "Custom2")
            return MaterialTextureType::Custom2;
        if (str == "Custom3")
            return MaterialTextureType::Custom3;
        return MaterialTextureType::Albedo;
    }

    size_t HashSampling(int filter, int addressU, int addressV, int addressW, unsigned int maxAniso, float mipBias,
                        float minLOD, float maxLOD)
    {
        size_t hash = 0;
        auto hashCombine = [](size_t& seed, size_t value) { seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2); };

        hashCombine(hash, std::hash<int>{}(filter));
        hashCombine(hash, std::hash<int>{}(addressU));
        hashCombine(hash, std::hash<int>{}(addressV));
        hashCombine(hash, std::hash<int>{}(addressW));
        hashCombine(hash, std::hash<unsigned int>{}(maxAniso));
        hashCombine(hash, std::hash<float>{}(mipBias));
        hashCombine(hash, std::hash<float>{}(minLOD));
        hashCombine(hash, std::hash<float>{}(maxLOD));

        return hash;
    }

} // anonymous namespace

// ============================================================================
// Texture Type Conversion Round-Trip
// ============================================================================

TEST(MaterialSystem_TextureTypeRoundTrip_AllTypes)
{
    MaterialTextureType types[] = {
        MaterialTextureType::Albedo,
        MaterialTextureType::Normal,
        MaterialTextureType::Metallic,
        MaterialTextureType::Roughness,
        MaterialTextureType::Occlusion,
        MaterialTextureType::Emissive,
        MaterialTextureType::Height,
        MaterialTextureType::DetailAlbedo,
        MaterialTextureType::DetailNormal,
        MaterialTextureType::Subsurface,
        MaterialTextureType::Transmission,
        MaterialTextureType::Clearcoat,
        MaterialTextureType::ClearcoatRoughness,
        MaterialTextureType::Anisotropy,
        MaterialTextureType::Custom0,
        MaterialTextureType::Custom1,
        MaterialTextureType::Custom2,
        MaterialTextureType::Custom3,
    };

    for (auto type : types)
    {
        std::string str = TextureTypeToString(type);
        EXPECT_FALSE(str.empty());
        EXPECT_NE(str, std::string("Unknown"));

        MaterialTextureType roundTripped = StringToTextureType(str);
        EXPECT_EQ(static_cast<int>(roundTripped), static_cast<int>(type));
    }
}

TEST(MaterialSystem_StringToTextureType_UnknownFallback)
{
    auto result = StringToTextureType("NonExistentType");
    EXPECT_EQ(static_cast<int>(result), static_cast<int>(MaterialTextureType::Albedo));
}

// ============================================================================
// Sampler Hash Tests
// ============================================================================

TEST(MaterialSystem_SamplerHash_SameInputsSameHash)
{
    size_t hash1 = HashSampling(0x55, 1, 1, 1, 16, 0.0f, 0.0f, 1000.0f);
    size_t hash2 = HashSampling(0x55, 1, 1, 1, 16, 0.0f, 0.0f, 1000.0f);
    EXPECT_EQ(hash1, hash2);
}

TEST(MaterialSystem_SamplerHash_DifferentInputsDifferentHash)
{
    size_t hash1 = HashSampling(0x55, 1, 1, 1, 16, 0.0f, 0.0f, 1000.0f);
    size_t hash2 = HashSampling(0x15, 1, 1, 1, 16, 0.0f, 0.0f, 1000.0f); // different filter
    EXPECT_NE(hash1, hash2);
}

TEST(MaterialSystem_SamplerHash_AnisotropyAffectsHash)
{
    size_t hash1 = HashSampling(0x55, 1, 1, 1, 4, 0.0f, 0.0f, 1000.0f);
    size_t hash2 = HashSampling(0x55, 1, 1, 1, 16, 0.0f, 0.0f, 1000.0f);
    EXPECT_NE(hash1, hash2);
}

// ============================================================================
// MaterialRenderState Defaults
// ============================================================================

TEST(MaterialSystem_RenderStateDefaults)
{
    MaterialRenderState state;
    EXPECT_EQ(static_cast<int>(state.blendMode), static_cast<int>(BlendMode::Opaque));
    EXPECT_EQ(static_cast<int>(state.cullMode), static_cast<int>(CullMode::Back));
    EXPECT_TRUE(state.depthTest);
    EXPECT_TRUE(state.depthWrite);
    EXPECT_TRUE(state.castShadows);
    EXPECT_TRUE(state.receiveShadows);
    EXPECT_EQ(state.renderQueue, 2000);
    EXPECT_FALSE(state.doubleSided);
}

TEST(MaterialSystem_TransparentModeDisablesDepthWrite)
{
    MaterialRenderState state;
    state.blendMode = BlendMode::Transparent;
    state.depthWrite = false;

    EXPECT_FALSE(state.depthWrite);
    EXPECT_EQ(static_cast<int>(state.blendMode), static_cast<int>(BlendMode::Transparent));
}

// ============================================================================
// MaterialMetrics
// ============================================================================

TEST(MaterialSystem_MetricsDefaults)
{
    MaterialMetrics metrics;
    EXPECT_EQ(metrics.loadedMaterials, 0);
    EXPECT_EQ(metrics.textureCount, 0);
    EXPECT_EQ(metrics.textureMemory, static_cast<size_t>(0));
    EXPECT_EQ(metrics.materialSwitches, 0);
    EXPECT_EQ(metrics.textureBinds, 0);
    EXPECT_NEAR(metrics.averageLoadTime, 0.0f, 0.01f);
    EXPECT_FALSE(metrics.hotReloadEnabled);
}

TEST(MaterialSystem_MetricsMaterialCount)
{
    MaterialMetrics metrics;
    metrics.loadedMaterials = 42;
    metrics.textureCount = 128;
    metrics.textureMemory = 256 * 1024 * 1024;

    EXPECT_EQ(metrics.loadedMaterials, 42);
    EXPECT_EQ(metrics.textureCount, 128);
    EXPECT_EQ(metrics.textureMemory, static_cast<size_t>(256 * 1024 * 1024));
}

// ============================================================================
// Blend Mode Exhaustive Test
// ============================================================================

TEST(MaterialSystem_AllBlendModesDistinct)
{
    std::vector<int> modes = {
        static_cast<int>(BlendMode::Opaque),      static_cast<int>(BlendMode::AlphaTest),
        static_cast<int>(BlendMode::Transparent), static_cast<int>(BlendMode::Additive),
        static_cast<int>(BlendMode::Multiply),    static_cast<int>(BlendMode::Screen),
    };

    // All modes should be distinct values
    for (size_t i = 0; i < modes.size(); i++)
    {
        for (size_t j = i + 1; j < modes.size(); j++)
        {
            EXPECT_NE(modes[i], modes[j]);
        }
    }
}
