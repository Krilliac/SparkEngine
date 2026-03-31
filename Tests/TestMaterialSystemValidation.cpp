/**
 * @file TestMaterialSystemValidation.cpp
 * @brief Tests for MaterialSystem: PBR property validation, material instancing,
 *        variant management, shader permutations, and material serialization.
 *
 * Standalone reimplementation — mirrors Spark::Material and MaterialSystem
 * without D3D11/GPU dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // --- Enums mirroring MaterialSystem.h ---

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
        Count
    };

    // --- PBR Properties ---

    struct PBRProperties
    {
        float albedoColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float metallicFactor = 0.0f;
        float roughnessFactor = 0.5f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
        float emissiveFactor = 1.0f;
        float alphaCutoff = 0.5f;
        float IOR = 1.5f;
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

    struct MaterialTexture
    {
        bool enabled = false;
        std::string filePath;
        float intensity = 1.0f;
        float tilingU = 1.0f;
        float tilingV = 1.0f;
        float offsetU = 0.0f;
        float offsetV = 0.0f;
    };

    // --- PBR Validation ---

    static bool ValidatePBRProperties(const PBRProperties& props)
    {
        if (props.metallicFactor < 0.0f || props.metallicFactor > 1.0f)
            return false;
        if (props.roughnessFactor < 0.0f || props.roughnessFactor > 1.0f)
            return false;
        if (props.normalScale < 0.0f)
            return false;
        if (props.occlusionStrength < 0.0f || props.occlusionStrength > 1.0f)
            return false;
        if (props.alphaCutoff < 0.0f || props.alphaCutoff > 1.0f)
            return false;
        if (props.IOR < 1.0f)
            return false;
        for (int i = 0; i < 4; i++)
        {
            if (props.albedoColor[i] < 0.0f || props.albedoColor[i] > 1.0f)
                return false;
        }
        for (int i = 0; i < 3; i++)
        {
            if (props.emissiveColor[i] < 0.0f)
                return false;
        }
        return true;
    }

    // --- Material class ---

    class Material
    {
      public:
        explicit Material(const std::string& name) : m_name(name) {}

        const std::string& GetName() const { return m_name; }
        const PBRProperties& GetPBRProperties() const { return m_pbrProps; }
        const MaterialRenderState& GetRenderState() const { return m_renderState; }

        void SetPBRProperties(const PBRProperties& props) { m_pbrProps = props; }
        void SetRenderState(const MaterialRenderState& state) { m_renderState = state; }

        void SetTexture(MaterialTextureType type, const MaterialTexture& tex)
        {
            m_textures[static_cast<int>(type)] = tex;
        }

        const MaterialTexture& GetTexture(MaterialTextureType type) const { return m_textures[static_cast<int>(type)]; }

        bool HasTexture(MaterialTextureType type) const { return m_textures[static_cast<int>(type)].enabled; }

        void UnloadTexture(MaterialTextureType type) { m_textures[static_cast<int>(type)] = MaterialTexture{}; }

        // Variant management
        void CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
        {
            m_variants[variantName] = defines;
            if (m_activeVariant.empty())
                m_activeVariant = variantName;
        }

        void SetActiveVariant(const std::string& variantName)
        {
            if (m_variants.count(variantName))
                m_activeVariant = variantName;
        }

        const std::string& GetActiveVariant() const { return m_activeVariant; }

        std::vector<std::string> GetAvailableVariants() const
        {
            std::vector<std::string> result;
            for (auto& [name, _] : m_variants)
                result.push_back(name);
            return result;
        }

        // Shader permutations
        std::vector<std::string> GetShaderPermutation() const
        {
            std::vector<std::string> perms;
            if (HasTexture(MaterialTextureType::Normal))
                perms.push_back("HAS_NORMAL_MAP");
            if (HasTexture(MaterialTextureType::Emissive))
                perms.push_back("HAS_EMISSIVE");
            if (m_renderState.blendMode == BlendMode::AlphaTest)
                perms.push_back("ALPHA_TEST");
            if (m_renderState.blendMode == BlendMode::Transparent)
                perms.push_back("ALPHA_BLEND");
            if (m_renderState.doubleSided)
                perms.push_back("DOUBLE_SIDED");

            // Add variant defines
            if (!m_activeVariant.empty())
            {
                auto it = m_variants.find(m_activeVariant);
                if (it != m_variants.end())
                {
                    for (auto& def : it->second)
                        perms.push_back(def);
                }
            }
            return perms;
        }

        // Instancing
        std::shared_ptr<Material> CreateInstance(const std::string& instanceName) const
        {
            auto inst = std::make_shared<Material>(instanceName);
            inst->m_pbrProps = m_pbrProps;
            inst->m_renderState = m_renderState;
            inst->m_textures = m_textures;
            inst->m_variants = m_variants;
            inst->m_activeVariant = m_activeVariant;
            inst->m_parentName = m_name;
            return inst;
        }

        const std::string& GetParentName() const { return m_parentName; }

        // Simple serialization
        std::string Serialize() const
        {
            std::string result;
            result += "name=" + m_name + "\n";
            result += "metallic=" + std::to_string(m_pbrProps.metallicFactor) + "\n";
            result += "roughness=" + std::to_string(m_pbrProps.roughnessFactor) + "\n";
            result += "blendMode=" + std::to_string(static_cast<int>(m_renderState.blendMode)) + "\n";
            result += "cullMode=" + std::to_string(static_cast<int>(m_renderState.cullMode)) + "\n";
            return result;
        }

      private:
        std::string m_name;
        std::string m_parentName;
        PBRProperties m_pbrProps;
        MaterialRenderState m_renderState;
        std::array<MaterialTexture, static_cast<int>(MaterialTextureType::Count)> m_textures{};
        std::unordered_map<std::string, std::vector<std::string>> m_variants;
        std::string m_activeVariant;
    };

    // --- MaterialSystem ---

    class MaterialSystem
    {
      public:
        std::shared_ptr<Material> CreateMaterial(const std::string& name)
        {
            auto mat = std::make_shared<Material>(name);
            m_materials[name] = mat;
            return mat;
        }

        std::shared_ptr<Material> GetMaterial(const std::string& name) const
        {
            auto it = m_materials.find(name);
            return (it != m_materials.end()) ? it->second : nullptr;
        }

        void UnloadMaterial(const std::string& name) { m_materials.erase(name); }

        void UnloadAllMaterials() { m_materials.clear(); }

        std::shared_ptr<Material> CreateMaterialInstance(const std::string& templateName,
                                                         const std::string& instanceName)
        {
            auto tmpl = GetMaterial(templateName);
            if (!tmpl)
                return nullptr;
            auto instance = tmpl->CreateInstance(instanceName);
            m_materials[instanceName] = instance;
            return instance;
        }

        size_t GetMaterialCount() const { return m_materials.size(); }

      private:
        std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    };

} // anonymous namespace

// ============================================================================
// PBR Validation Tests
// ============================================================================

TEST(MaterialSystem_ValidPBRDefaults)
{
    PBRProperties props;
    EXPECT_TRUE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_MetallicOutOfRange)
{
    PBRProperties props;
    props.metallicFactor = 1.5f;
    EXPECT_FALSE(ValidatePBRProperties(props));

    props.metallicFactor = -0.1f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_RoughnessOutOfRange)
{
    PBRProperties props;
    props.roughnessFactor = 2.0f;
    EXPECT_FALSE(ValidatePBRProperties(props));

    props.roughnessFactor = -0.5f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_NormalScaleNegative)
{
    PBRProperties props;
    props.normalScale = -1.0f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_OcclusionOutOfRange)
{
    PBRProperties props;
    props.occlusionStrength = 1.1f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_AlphaCutoffOutOfRange)
{
    PBRProperties props;
    props.alphaCutoff = 1.5f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_IORBelowOne)
{
    PBRProperties props;
    props.IOR = 0.5f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_AlbedoColorOutOfRange)
{
    PBRProperties props;
    props.albedoColor[0] = 1.5f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_EmissiveNegative)
{
    PBRProperties props;
    props.emissiveColor[1] = -0.1f;
    EXPECT_FALSE(ValidatePBRProperties(props));
}

TEST(MaterialSystem_ValidBoundaryValues)
{
    PBRProperties props;
    props.metallicFactor = 0.0f;
    props.roughnessFactor = 1.0f;
    props.normalScale = 0.0f;
    props.occlusionStrength = 0.0f;
    props.alphaCutoff = 0.0f;
    props.IOR = 1.0f;
    EXPECT_TRUE(ValidatePBRProperties(props));
}

// ============================================================================
// Material Instancing Tests
// ============================================================================

TEST(MaterialSystem_CreateInstance)
{
    Material base("BaseMetal");
    PBRProperties props;
    props.metallicFactor = 1.0f;
    props.roughnessFactor = 0.2f;
    base.SetPBRProperties(props);

    auto instance = base.CreateInstance("RustyMetal");

    EXPECT_EQ(instance->GetName(), std::string("RustyMetal"));
    EXPECT_EQ(instance->GetParentName(), std::string("BaseMetal"));
    EXPECT_NEAR(instance->GetPBRProperties().metallicFactor, 1.0f, 0.001f);
    EXPECT_NEAR(instance->GetPBRProperties().roughnessFactor, 0.2f, 0.001f);
}

TEST(MaterialSystem_InstanceIndependent)
{
    Material base("Base");
    auto instance = base.CreateInstance("Instance");

    // Modify instance — should not affect base
    PBRProperties modified;
    modified.metallicFactor = 0.8f;
    instance->SetPBRProperties(modified);

    EXPECT_NEAR(base.GetPBRProperties().metallicFactor, 0.0f, 0.001f);
    EXPECT_NEAR(instance->GetPBRProperties().metallicFactor, 0.8f, 0.001f);
}

TEST(MaterialSystem_SystemCreateInstance)
{
    MaterialSystem sys;
    auto base = sys.CreateMaterial("Template");
    PBRProperties props;
    props.metallicFactor = 0.9f;
    base->SetPBRProperties(props);

    auto instance = sys.CreateMaterialInstance("Template", "Instance1");
    EXPECT_TRUE(instance != nullptr);
    EXPECT_NEAR(instance->GetPBRProperties().metallicFactor, 0.9f, 0.001f);
    EXPECT_EQ(sys.GetMaterialCount(), (size_t)2);
}

TEST(MaterialSystem_CreateInstanceFromMissing)
{
    MaterialSystem sys;
    auto result = sys.CreateMaterialInstance("NonExistent", "Instance");
    EXPECT_TRUE(result == nullptr);
}

// ============================================================================
// Variant Management Tests
// ============================================================================

TEST(MaterialSystem_CreateVariant)
{
    Material mat("TestMat");
    mat.CreateVariant("Wet", {"WETNESS_ON", "RAIN_PARTICLES"});
    mat.CreateVariant("Snow", {"SNOW_OVERLAY"});

    auto variants = mat.GetAvailableVariants();
    EXPECT_EQ(variants.size(), (size_t)2);
}

TEST(MaterialSystem_SetActiveVariant)
{
    Material mat("TestMat");
    mat.CreateVariant("Default", {});
    mat.CreateVariant("Wet", {"WETNESS_ON"});

    mat.SetActiveVariant("Wet");
    EXPECT_EQ(mat.GetActiveVariant(), std::string("Wet"));
}

TEST(MaterialSystem_InvalidVariantIgnored)
{
    Material mat("TestMat");
    mat.CreateVariant("Default", {});
    mat.SetActiveVariant("Default");
    mat.SetActiveVariant("NonExistent");
    EXPECT_EQ(mat.GetActiveVariant(), std::string("Default"));
}

// ============================================================================
// Shader Permutation Tests
// ============================================================================

TEST(MaterialSystem_ShaderPermutationNormalMap)
{
    Material mat("TestMat");
    MaterialTexture normalTex;
    normalTex.enabled = true;
    normalTex.filePath = "textures/normal.png";
    mat.SetTexture(MaterialTextureType::Normal, normalTex);

    auto perms = mat.GetShaderPermutation();
    bool hasNormalMap = false;
    for (auto& p : perms)
    {
        if (p == "HAS_NORMAL_MAP")
            hasNormalMap = true;
    }
    EXPECT_TRUE(hasNormalMap);
}

TEST(MaterialSystem_ShaderPermutationAlphaTest)
{
    Material mat("TestMat");
    MaterialRenderState state;
    state.blendMode = BlendMode::AlphaTest;
    mat.SetRenderState(state);

    auto perms = mat.GetShaderPermutation();
    bool hasAlphaTest = false;
    for (auto& p : perms)
    {
        if (p == "ALPHA_TEST")
            hasAlphaTest = true;
    }
    EXPECT_TRUE(hasAlphaTest);
}

TEST(MaterialSystem_ShaderPermutationVariantDefines)
{
    Material mat("TestMat");
    mat.CreateVariant("Wet", {"WETNESS_ON"});
    mat.SetActiveVariant("Wet");

    auto perms = mat.GetShaderPermutation();
    bool hasWetness = false;
    for (auto& p : perms)
    {
        if (p == "WETNESS_ON")
            hasWetness = true;
    }
    EXPECT_TRUE(hasWetness);
}

TEST(MaterialSystem_ShaderPermutationDoubleSided)
{
    Material mat("TestMat");
    MaterialRenderState state;
    state.doubleSided = true;
    mat.SetRenderState(state);

    auto perms = mat.GetShaderPermutation();
    bool hasDoubleSided = false;
    for (auto& p : perms)
    {
        if (p == "DOUBLE_SIDED")
            hasDoubleSided = true;
    }
    EXPECT_TRUE(hasDoubleSided);
}

// ============================================================================
// Texture Management Tests
// ============================================================================

TEST(MaterialSystem_SetAndGetTexture)
{
    Material mat("TestMat");
    MaterialTexture tex;
    tex.enabled = true;
    tex.filePath = "textures/albedo.png";
    tex.intensity = 0.8f;
    mat.SetTexture(MaterialTextureType::Albedo, tex);

    EXPECT_TRUE(mat.HasTexture(MaterialTextureType::Albedo));
    EXPECT_EQ(mat.GetTexture(MaterialTextureType::Albedo).filePath, std::string("textures/albedo.png"));
    EXPECT_NEAR(mat.GetTexture(MaterialTextureType::Albedo).intensity, 0.8f, 0.001f);
}

TEST(MaterialSystem_UnloadTexture)
{
    Material mat("TestMat");
    MaterialTexture tex;
    tex.enabled = true;
    mat.SetTexture(MaterialTextureType::Albedo, tex);
    EXPECT_TRUE(mat.HasTexture(MaterialTextureType::Albedo));

    mat.UnloadTexture(MaterialTextureType::Albedo);
    EXPECT_FALSE(mat.HasTexture(MaterialTextureType::Albedo));
}

// ============================================================================
// MaterialSystem Management Tests
// ============================================================================

TEST(MaterialSystem_CreateAndRetrieve)
{
    MaterialSystem sys;
    auto mat = sys.CreateMaterial("TestMat");
    EXPECT_TRUE(mat != nullptr);

    auto retrieved = sys.GetMaterial("TestMat");
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->GetName(), std::string("TestMat"));
}

TEST(MaterialSystem_GetMissing)
{
    MaterialSystem sys;
    EXPECT_TRUE(sys.GetMaterial("DoesNotExist") == nullptr);
}

TEST(MaterialSystem_UnloadMaterial)
{
    MaterialSystem sys;
    sys.CreateMaterial("Mat1");
    sys.CreateMaterial("Mat2");
    EXPECT_EQ(sys.GetMaterialCount(), (size_t)2);

    sys.UnloadMaterial("Mat1");
    EXPECT_EQ(sys.GetMaterialCount(), (size_t)1);
    EXPECT_TRUE(sys.GetMaterial("Mat1") == nullptr);
}

TEST(MaterialSystem_UnloadAll)
{
    MaterialSystem sys;
    sys.CreateMaterial("Mat1");
    sys.CreateMaterial("Mat2");
    sys.UnloadAllMaterials();
    EXPECT_EQ(sys.GetMaterialCount(), (size_t)0);
}

// ============================================================================
// Render State Tests
// ============================================================================

TEST(MaterialSystem_DefaultRenderState)
{
    Material mat("TestMat");
    auto& state = mat.GetRenderState();
    EXPECT_EQ(static_cast<int>(state.blendMode), static_cast<int>(BlendMode::Opaque));
    EXPECT_EQ(static_cast<int>(state.cullMode), static_cast<int>(CullMode::Back));
    EXPECT_TRUE(state.depthTest);
    EXPECT_TRUE(state.depthWrite);
    EXPECT_TRUE(state.castShadows);
}

TEST(MaterialSystem_TransparentDisablesDepthWrite)
{
    Material mat("Glass");
    MaterialRenderState state;
    state.blendMode = BlendMode::Transparent;
    state.depthWrite = false; // typical for transparent
    mat.SetRenderState(state);

    EXPECT_EQ(static_cast<int>(mat.GetRenderState().blendMode), static_cast<int>(BlendMode::Transparent));
    EXPECT_FALSE(mat.GetRenderState().depthWrite);
}

// ============================================================================
// Serialization Tests
// ============================================================================

TEST(MaterialSystem_SerializeContainsName)
{
    Material mat("PBRMetal");
    auto serialized = mat.Serialize();
    EXPECT_TRUE(serialized.find("name=PBRMetal") != std::string::npos);
}

TEST(MaterialSystem_SerializeContainsPBRValues)
{
    Material mat("TestMat");
    PBRProperties props;
    props.metallicFactor = 0.75f;
    props.roughnessFactor = 0.3f;
    mat.SetPBRProperties(props);

    auto serialized = mat.Serialize();
    EXPECT_TRUE(serialized.find("metallic=0.7") != std::string::npos);
    EXPECT_TRUE(serialized.find("roughness=0.3") != std::string::npos);
}
