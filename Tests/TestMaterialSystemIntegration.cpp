/**
 * @file TestMaterialSystemIntegration.cpp
 * @brief Integration tests for MaterialSystem construction and Material lifecycle
 *
 * Tests Material object creation, property management, render state, and
 * variant management without requiring a D3D11 device. MaterialSystem::Initialize
 * requires a device, so we test standalone Material objects and the persistent CB.
 */

#include "TestFramework.h"
#include "Graphics/MaterialSystem.h"

// ============================================================================
// Material Object Construction Tests
// ============================================================================

TEST(MaterialInteg_ConstructDefaultMaterial)
{
    Material mat("TestMaterial");
    EXPECT_EQ(mat.GetName(), std::string("TestMaterial"));
}

TEST(MaterialInteg_DefaultPBRProperties)
{
    Material mat("PBRTest");
    const auto& props = mat.GetPBRProperties();
    // Default PBR should be valid
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

TEST(MaterialInteg_SetGetPBRProperties)
{
    Material mat("PBRSet");
    PBRProperties props;
    props.metallicFactor = 0.8f;
    props.roughnessFactor = 0.2f;
    props.normalScale = 1.0f;
    props.occlusionStrength = 0.9f;
    props.alphaCutoff = 0.5f;
    props.indexOfRefraction = 1.5f;
    props.emissiveFactor = 0.0f;

    mat.SetPBRProperties(props);

    const auto& result = mat.GetPBRProperties();
    EXPECT_NEAR(result.metallicFactor, 0.8f, 0.001f);
    EXPECT_NEAR(result.roughnessFactor, 0.2f, 0.001f);
    EXPECT_NEAR(result.occlusionStrength, 0.9f, 0.001f);
}

// ============================================================================
// PBR Validation Edge Cases
// ============================================================================

TEST(MaterialInteg_PBR_ZeroMetallicZeroRoughness_Valid)
{
    PBRProperties props;
    props.metallicFactor = 0.0f;
    props.roughnessFactor = 0.0f;
    props.normalScale = 1.0f;
    props.occlusionStrength = 0.0f;
    props.alphaCutoff = 0.0f;
    props.indexOfRefraction = 1.0f;
    props.emissiveFactor = 0.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

TEST(MaterialInteg_PBR_MaxMetallicMaxRoughness_Valid)
{
    PBRProperties props;
    props.metallicFactor = 1.0f;
    props.roughnessFactor = 1.0f;
    props.normalScale = 1.0f;
    props.occlusionStrength = 1.0f;
    props.alphaCutoff = 1.0f;
    props.indexOfRefraction = 2.5f;
    props.emissiveFactor = 10.0f;
    EXPECT_TRUE(Material::ValidatePBRProperties(props));
}

// ============================================================================
// Material Render State Tests
// ============================================================================

TEST(MaterialInteg_DefaultRenderState)
{
    Material mat("RenderStateTest");
    const auto& state = mat.GetRenderState();
    // Default should be opaque, backface culled, depth enabled
    EXPECT_EQ(static_cast<int>(state.blendMode), static_cast<int>(BlendMode::Opaque));
    EXPECT_EQ(static_cast<int>(state.cullMode), static_cast<int>(CullMode::Back));
    EXPECT_TRUE(state.depthWrite);
    EXPECT_TRUE(state.depthTest);
}

TEST(MaterialInteg_SetRenderState_Transparent)
{
    Material mat("Transparent");
    MaterialRenderState state;
    state.blendMode = BlendMode::Transparent;
    state.cullMode = CullMode::None;
    state.depthWrite = false;
    state.depthTest = true;
    mat.SetRenderState(state);

    const auto& result = mat.GetRenderState();
    EXPECT_EQ(static_cast<int>(result.blendMode), static_cast<int>(BlendMode::Transparent));
    EXPECT_EQ(static_cast<int>(result.cullMode), static_cast<int>(CullMode::None));
    EXPECT_FALSE(result.depthWrite);
}

// ============================================================================
// Material Variant Tests
// ============================================================================

TEST(MaterialInteg_GetAvailableVariants_DoesNotCrash)
{
    Material mat("VariantTest");
    auto variants = mat.GetAvailableVariants();
    // May be empty if no variants registered — just verify no crash
    EXPECT_TRUE(variants.size() >= 0u);
}

// ============================================================================
// Persistent Material CB Manager Tests
// ============================================================================

TEST(PersistentCB_InitializeAndQuery)
{
    Spark::Graphics::PersistentMaterialCBManager cb;
    cb.Initialize(128, 64);
    EXPECT_EQ(cb.GetDirtyCount(), 0u);
}

TEST(PersistentCB_RegisterAndUpdate)
{
    Spark::Graphics::PersistentMaterialCBManager cb;
    cb.Initialize(128, 64);

    uint32_t slot = cb.RegisterMaterial(1);
    EXPECT_TRUE(slot != UINT32_MAX);

    // Write some data to the material slot
    std::vector<uint8_t> data(64, 0xAA);
    bool changed = cb.UpdateMaterial(1, data.data(), static_cast<uint32_t>(data.size()));

    EXPECT_TRUE(changed);
    EXPECT_EQ(cb.GetDirtyCount(), 1u);
}

TEST(PersistentCB_ClearDirtyFlags)
{
    Spark::Graphics::PersistentMaterialCBManager cb;
    cb.Initialize(128, 64);

    cb.RegisterMaterial(1);
    std::vector<uint8_t> data(64, 0xBB);
    cb.UpdateMaterial(1, data.data(), static_cast<uint32_t>(data.size()));
    EXPECT_EQ(cb.GetDirtyCount(), 1u);

    cb.ClearDirtyFlags();
    EXPECT_EQ(cb.GetDirtyCount(), 0u);
}

TEST(PersistentCB_UpdateUnchangedData_NotDirty)
{
    Spark::Graphics::PersistentMaterialCBManager cb;
    cb.Initialize(128, 64);

    cb.RegisterMaterial(1);
    std::vector<uint8_t> data(64, 0xCC);
    cb.UpdateMaterial(1, data.data(), static_cast<uint32_t>(data.size()));
    cb.ClearDirtyFlags();

    // Same data again — should NOT mark dirty
    bool changed = cb.UpdateMaterial(1, data.data(), static_cast<uint32_t>(data.size()));
    EXPECT_FALSE(changed);
    EXPECT_EQ(cb.GetDirtyCount(), 0u);
}

TEST(PersistentCB_MultipleRegistrations)
{
    Spark::Graphics::PersistentMaterialCBManager cb;
    cb.Initialize(64, 32);

    uint32_t s0 = cb.RegisterMaterial(10);
    uint32_t s1 = cb.RegisterMaterial(20);
    uint32_t s2 = cb.RegisterMaterial(30);

    EXPECT_TRUE(s0 != s1);
    EXPECT_TRUE(s1 != s2);
    EXPECT_TRUE(s0 != s2);
}

// ============================================================================
// MaterialSystem Construction Test (no Initialize — needs device)
// ============================================================================

TEST(MaterialSystem_ConstructDestruct_DoesNotCrash)
{
    // Just verify the constructor/destructor pair doesn't crash
    // (Initialize requires a device, so we can't test the full lifecycle on Linux)
    MaterialSystem system;
}
