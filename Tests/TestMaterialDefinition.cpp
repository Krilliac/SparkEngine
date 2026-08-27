// TestMaterialDefinition.cpp - Tests for the declarative material definition format

#include "TestFramework.h"
#include "Graphics/MaterialDefinition.h"

using namespace Spark::Graphics;

TEST(MaterialDefinition_Construction)
{
    MaterialDefinition def;
    def.name = "TestPBR";
    def.shadingModel = ShadingModel::Lit;

    def.AddParameter("metallic", 0.0f, 0.0f, 1.0f, "Metallic factor");
    def.AddParameter("roughness", 0.5f, 0.0f, 1.0f, "Roughness factor");
    def.AddFloat4Parameter("baseColor", {1.0f, 1.0f, 1.0f, 1.0f}, "Base color");
    def.AddBoolParameter("doubleSided", false, "Double-sided rendering");

    EXPECT_EQ(def.parameters.size(), static_cast<size_t>(4));
    EXPECT_TRUE(def.name == "TestPBR");

    auto* metallic = def.FindParameter("metallic");
    ASSERT_TRUE(metallic != nullptr);
    ASSERT_TRUE(metallic->minValue.has_value());
    ASSERT_TRUE(metallic->maxValue.has_value());
    EXPECT_NEAR(*metallic->minValue, 0.0f, 1e-6f);
    EXPECT_NEAR(*metallic->maxValue, 1.0f, 1e-6f);

    EXPECT_TRUE(def.FindParameter("nonexistent") == nullptr);
}

TEST(MaterialDefinition_TextureSlots)
{
    MaterialDefinition def;
    def.name = "TexturedPBR";

    def.AddTextureSlot("albedoMap", TextureSlotUsage::Albedo, true, "Base color texture");
    def.AddTextureSlot("normalMap", TextureSlotUsage::Normal, false, "Normal map");

    EXPECT_EQ(def.textureSlots.size(), static_cast<size_t>(2));

    auto* albedo = def.FindTextureSlot("albedoMap");
    ASSERT_TRUE(albedo != nullptr);
    EXPECT_TRUE(albedo->required);

    auto* normal = def.FindTextureSlot("normalMap");
    ASSERT_TRUE(normal != nullptr);
    EXPECT_FALSE(normal->required);

    EXPECT_TRUE(def.FindTextureSlot("missing") == nullptr);
}

TEST(MaterialDefinition_ShaderDefines)
{
    MaterialDefinition def;
    def.name = "SubsurfaceMat";
    def.shadingModel = ShadingModel::Subsurface;
    def.requiredFeatures.push_back("CLEARCOAT");

    def.AddTextureSlot("normalMap", TextureSlotUsage::Normal);
    def.AddTextureSlot("emissiveMap", TextureSlotUsage::Emissive);

    auto defines = def.GetShaderDefines();

    bool hasSubsurface = false;
    bool hasNormal = false;
    bool hasEmissive = false;
    bool hasClearcoat = false;

    for (const auto& d : defines)
    {
        if (d == "SHADING_MODEL_SUBSURFACE")
            hasSubsurface = true;
        if (d == "HAS_NORMAL_MAP")
            hasNormal = true;
        if (d == "HAS_EMISSIVE_MAP")
            hasEmissive = true;
        if (d == "CLEARCOAT")
            hasClearcoat = true;
    }

    EXPECT_TRUE(hasSubsurface);
    EXPECT_TRUE(hasNormal);
    EXPECT_TRUE(hasEmissive);
    EXPECT_TRUE(hasClearcoat);
}

TEST(MaterialDefinition_InstanceOverrides)
{
    MaterialDefinition def;
    def.name = "TestMat";
    def.AddParameter("metallic", 0.0f, 0.0f, 1.0f);
    def.AddParameter("roughness", 0.5f, 0.0f, 1.0f);
    def.AddFloat4Parameter("baseColor", {1.0f, 1.0f, 1.0f, 1.0f});

    MaterialInstance instance(def, "instance1");

    EXPECT_NEAR(instance.GetFloat("metallic"), 0.0f, 1e-6f);
    EXPECT_NEAR(instance.GetFloat("roughness"), 0.5f, 1e-6f);
    EXPECT_FALSE(instance.HasOverrides());

    EXPECT_TRUE(instance.SetFloat("metallic", 0.8f));
    EXPECT_NEAR(instance.GetFloat("metallic"), 0.8f, 1e-6f);
    EXPECT_TRUE(instance.HasOverrides());

    // Out-of-range should fail
    EXPECT_FALSE(instance.SetFloat("metallic", 1.5f));
    EXPECT_FALSE(instance.SetFloat("metallic", -0.1f));

    // Wrong type should fail
    EXPECT_FALSE(instance.SetFloat("baseColor", 1.0f));

    // Nonexistent should fail
    EXPECT_FALSE(instance.SetFloat("nonexistent", 0.5f));
}

TEST(MaterialDefinition_Validation)
{
    MaterialDefinition def;
    def.name = "ValidatedMat";
    def.AddParameter("roughness", 0.5f, 0.0f, 1.0f);
    def.AddTextureSlot("albedoMap", TextureSlotUsage::Albedo, true);
    def.AddTextureSlot("normalMap", TextureSlotUsage::Normal, false);

    MaterialInstance instance(def);

    // Missing required texture
    auto errors = instance.Validate();
    EXPECT_EQ(errors.size(), static_cast<size_t>(1));

    // Assign required texture
    EXPECT_TRUE(instance.SetTexturePath("albedoMap", "textures/albedo.dds"));
    errors = instance.Validate();
    EXPECT_TRUE(errors.empty());

    // Invalid slot name
    EXPECT_FALSE(instance.SetTexturePath("nonexistent", "foo.dds"));
}

TEST(MaterialDefinition_ResetToDefaults)
{
    MaterialDefinition def;
    def.name = "ResetMat";
    def.AddParameter("metallic", 0.0f, 0.0f, 1.0f);
    def.AddTextureSlot("albedo", TextureSlotUsage::Albedo);

    MaterialInstance instance(def);
    instance.SetFloat("metallic", 0.9f);
    instance.SetTexturePath("albedo", "test.dds");
    EXPECT_TRUE(instance.HasOverrides());

    instance.ResetToDefaults();
    EXPECT_FALSE(instance.HasOverrides());
    EXPECT_NEAR(instance.GetFloat("metallic"), 0.0f, 1e-6f);
}

TEST(MaterialDefinition_Registry)
{
    MaterialDefinitionRegistry& registry = MaterialDefinitionRegistry::Get();
    registry.Clear();

    MaterialDefinition def1;
    def1.name = "StandardPBR";
    def1.shadingModel = ShadingModel::Lit;
    def1.AddParameter("metallic", 0.0f, 0.0f, 1.0f);

    MaterialDefinition def2;
    def2.name = "UnlitEmissive";
    def2.shadingModel = ShadingModel::Unlit;

    EXPECT_TRUE(registry.Register(std::move(def1)));
    EXPECT_TRUE(registry.Register(std::move(def2)));
    EXPECT_EQ(registry.Count(), static_cast<size_t>(2));

    // Duplicate should fail
    MaterialDefinition dup;
    dup.name = "StandardPBR";
    EXPECT_FALSE(registry.Register(std::move(dup)));

    auto* found = registry.Find("StandardPBR");
    EXPECT_TRUE(found != nullptr);
    EXPECT_TRUE(registry.Find("Nonexistent") == nullptr);

    auto names = registry.GetAllNames();
    EXPECT_EQ(names.size(), static_cast<size_t>(2));

    auto instance = registry.CreateInstance("StandardPBR", "myMaterial");
    EXPECT_TRUE(instance.has_value());
    EXPECT_NEAR(instance->GetFloat("metallic"), 0.0f, 1e-6f);

    auto badInstance = registry.CreateInstance("Nonexistent", "bad");
    EXPECT_FALSE(badInstance.has_value());

    registry.Clear();
    EXPECT_EQ(registry.Count(), static_cast<size_t>(0));
}

TEST(MaterialDefinition_BoolParameter)
{
    MaterialDefinition def;
    def.name = "BoolTest";
    def.AddBoolParameter("doubleSided", false, "Enable double-sided");

    MaterialInstance instance(def);
    EXPECT_FALSE(instance.GetBool("doubleSided"));

    EXPECT_TRUE(instance.SetBool("doubleSided", true));
    EXPECT_TRUE(instance.GetBool("doubleSided"));

    // Wrong type
    EXPECT_FALSE(instance.SetFloat("doubleSided", 1.0f));
}

TEST(MaterialDefinition_RenderStateDefaults)
{
    MaterialDefinition def;
    def.name = "DefaultState";

    EXPECT_TRUE(def.renderState.depthWrite);
    EXPECT_TRUE(def.renderState.depthTest);
    EXPECT_TRUE(def.renderState.castShadows);
    EXPECT_TRUE(def.renderState.receiveShadows);
    EXPECT_FALSE(def.renderState.doubleSided);
    EXPECT_NEAR(def.renderState.alphaCutoff, 0.5f, 1e-6f);
    EXPECT_EQ(def.renderState.renderQueue, 2000);
}

TEST(MaterialDefinition_TexturePathFallback)
{
    MaterialDefinition def;
    def.name = "TexFallback";

    TextureSlotDecl slot;
    slot.name = "albedo";
    slot.usage = TextureSlotUsage::Albedo;
    slot.defaultPath = "default_white.dds";
    def.textureSlots.push_back(slot);

    MaterialInstance instance(def);

    EXPECT_TRUE(instance.GetTexturePath("albedo") == "default_white.dds");

    instance.SetTexturePath("albedo", "custom.dds");
    EXPECT_TRUE(instance.GetTexturePath("albedo") == "custom.dds");

    EXPECT_TRUE(instance.GetTexturePath("nonexistent").empty());
}
