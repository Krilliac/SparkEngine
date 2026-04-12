/**
 * @file TestMaterialSystemReal.cpp
 * @brief Real-class tests for Material and MaterialSystem types
 *
 * Tests the actual production MaterialSystem.h — PBR validation,
 * material construction, enums, render state defaults, and advanced properties.
 */

#include "TestFramework.h"
#include "Graphics/MaterialSystem.h"

// ---------------------------------------------------------------------------
// PBR validation (static method)
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_ValidPBRPropertiesPass)
{
    PBRProperties props;
    props.metallicFactor = 0.5f;
    props.roughnessFactor = 0.5f;
    props.normalScale = 1.0f;
    props.occlusionStrength = 1.0f;
    props.alphaCutoff = 0.5f;
    props.indexOfRefraction = 1.5f;
    props.emissiveFactor = 0.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

TEST(MaterialSystemReal_PBRMetallicBoundaryValues)
{
    PBRProperties props;
    props.roughnessFactor = 0.5f;
    props.occlusionStrength = 1.0f;
    props.alphaCutoff = 0.5f;
    props.indexOfRefraction = 1.5f;
    props.emissiveFactor = 0.0f;

    props.metallicFactor = 0.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));

    props.metallicFactor = 1.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

TEST(MaterialSystemReal_PBRRoughnessBoundaryValues)
{
    PBRProperties props;
    props.metallicFactor = 0.5f;
    props.occlusionStrength = 1.0f;
    props.alphaCutoff = 0.5f;
    props.indexOfRefraction = 1.5f;
    props.emissiveFactor = 0.0f;

    props.roughnessFactor = 0.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));

    props.roughnessFactor = 1.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

// ---------------------------------------------------------------------------
// PBRProperties defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_PBRPropertiesDefaults)
{
    PBRProperties props;
    EXPECT_NEAR(props.metallicFactor, 0.0f, 0.001f);
    EXPECT_NEAR(props.roughnessFactor, 0.5f, 0.001f);
    EXPECT_NEAR(props.normalScale, 1.0f, 0.001f);
    EXPECT_NEAR(props.occlusionStrength, 1.0f, 0.001f);
    EXPECT_NEAR(props.alphaCutoff, 0.5f, 0.001f);
    EXPECT_NEAR(props.indexOfRefraction, 1.5f, 0.001f);
    EXPECT_NEAR(props.emissiveFactor, 0.0f, 0.001f);
    EXPECT_NEAR(props.albedoColor.x, 1.0f, 0.001f);
    EXPECT_NEAR(props.albedoColor.y, 1.0f, 0.001f);
    EXPECT_NEAR(props.albedoColor.z, 1.0f, 0.001f);
    EXPECT_NEAR(props.albedoColor.w, 1.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// Advanced properties defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_AdvancedPropertiesDefaults)
{
    AdvancedProperties adv;
    EXPECT_FALSE(adv.subsurfaceEnabled);
    EXPECT_FALSE(adv.clearcoatEnabled);
    EXPECT_FALSE(adv.anisotropyEnabled);
    EXPECT_FALSE(adv.transmissionEnabled);
    EXPECT_FALSE(adv.sheenEnabled);
    EXPECT_FALSE(adv.iridescenceEnabled);
    EXPECT_NEAR(adv.clearcoatFactor, 0.0f, 0.001f);
    EXPECT_NEAR(adv.clearcoatRoughness, 0.0f, 0.001f);
    EXPECT_NEAR(adv.anisotropyFactor, 0.0f, 0.001f);
    EXPECT_NEAR(adv.transmissionFactor, 0.0f, 0.001f);
    EXPECT_NEAR(adv.iridescenceFactor, 0.0f, 0.001f);
    EXPECT_NEAR(adv.iridescenceIOR, 1.3f, 0.001f);
    EXPECT_NEAR(adv.iridescenceThickness, 100.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// MaterialRenderState defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_RenderStateDefaults)
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

// ---------------------------------------------------------------------------
// BlendMode enum coverage
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_BlendModeEnumValues)
{
    EXPECT_EQ(static_cast<int>(BlendMode::Opaque), 0);
    EXPECT_EQ(static_cast<int>(BlendMode::AlphaTest), 1);
    EXPECT_EQ(static_cast<int>(BlendMode::Transparent), 2);
    EXPECT_EQ(static_cast<int>(BlendMode::Additive), 3);
    EXPECT_EQ(static_cast<int>(BlendMode::Multiply), 4);
    EXPECT_EQ(static_cast<int>(BlendMode::Screen), 5);
}

TEST(MaterialSystemReal_CullModeEnumValues)
{
    EXPECT_EQ(static_cast<int>(CullMode::None), 0);
    EXPECT_EQ(static_cast<int>(CullMode::Front), 1);
    EXPECT_EQ(static_cast<int>(CullMode::Back), 2);
}

// ---------------------------------------------------------------------------
// MaterialTextureType enum coverage
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_TextureTypeEnumDistinct)
{
    EXPECT_NE(static_cast<int>(MaterialTextureType::Albedo), static_cast<int>(MaterialTextureType::Normal));
    EXPECT_NE(static_cast<int>(MaterialTextureType::Metallic), static_cast<int>(MaterialTextureType::Roughness));
    EXPECT_NE(static_cast<int>(MaterialTextureType::Custom0), static_cast<int>(MaterialTextureType::Custom3));
}

// ---------------------------------------------------------------------------
// MaterialConstants alignment
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_ConstantsAlignment)
{
    EXPECT_EQ(alignof(MaterialConstants), size_t(16));
}

// ---------------------------------------------------------------------------
// TextureSampling defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_TextureSamplingDefaults)
{
    TextureSampling sampling;
    EXPECT_EQ(sampling.maxAnisotropy, 16u);
    EXPECT_NEAR(sampling.mipLODBias, 0.0f, 0.001f);
    EXPECT_NEAR(sampling.minLOD, 0.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// MaterialTexture defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_MaterialTextureDefaults)
{
    MaterialTexture tex;
    EXPECT_FALSE(tex.enabled);
    EXPECT_NEAR(tex.intensity, 1.0f, 0.001f);
    EXPECT_NEAR(tex.tiling.x, 1.0f, 0.001f);
    EXPECT_NEAR(tex.tiling.y, 1.0f, 0.001f);
    EXPECT_NEAR(tex.offset.x, 0.0f, 0.001f);
    EXPECT_NEAR(tex.offset.y, 0.0f, 0.001f);
    EXPECT_TRUE(tex.filePath.empty());
}

// ---------------------------------------------------------------------------
// MaterialMetrics defaults
// ---------------------------------------------------------------------------

TEST(MaterialSystemReal_MetricsDefaults)
{
    MaterialSystem::MaterialMetrics metrics{};
    EXPECT_EQ(metrics.loadedMaterials, 0);
    EXPECT_EQ(metrics.textureCount, 0);
    EXPECT_EQ(metrics.textureMemory, size_t(0));
    EXPECT_EQ(metrics.materialSwitches, 0);
}
