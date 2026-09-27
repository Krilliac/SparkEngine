/**
 * @file PBRMaterialLighting.cpp
 * @brief Platform-independent PBR material reflection and shader permutation logic
 *
 * Contains the reflection registrations for the PBR material structs and
 * GetShaderPermutation (shader define generation based on material state).
 * These are CPU-only and run identically on all platforms.
 *
 * Core material state is in PBRMaterial.cpp.
 * Shader binding is in PBRMaterialBinding.cpp.
 */

#include "MaterialSystem.h"
#include "../Core/Reflection.h"

// ============================================================================
// Reflection registrations for PBR material structs.
// ============================================================================

SPARK_REFLECT_TYPE(PBRProperties)
SPARK_REFLECT_FIELD_ATTR_AS(PBRProperties, albedoColor, "AlbedoColor", Spark::FieldType::Vector4, )
SPARK_REFLECT_FIELD(PBRProperties, metallicFactor, "MetallicFactor")
SPARK_REFLECT_FIELD(PBRProperties, roughnessFactor, "RoughnessFactor")
SPARK_REFLECT_FIELD(PBRProperties, normalScale, "NormalScale")
SPARK_REFLECT_FIELD(PBRProperties, occlusionStrength, "OcclusionStrength")
SPARK_REFLECT_FIELD_ATTR_AS(PBRProperties, emissiveColor, "EmissiveColor", Spark::FieldType::Vector3, )
SPARK_REFLECT_FIELD(PBRProperties, emissiveFactor, "EmissiveFactor")
SPARK_REFLECT_FIELD(PBRProperties, alphaCutoff, "AlphaCutoff")
SPARK_REFLECT_FIELD(PBRProperties, indexOfRefraction, "IndexOfRefraction")
SPARK_REFLECT_END(PBRProperties)

SPARK_REFLECT_TYPE(MaterialRenderState)
SPARK_REFLECT_FIELD(MaterialRenderState, blendMode, "BlendMode")
SPARK_REFLECT_FIELD(MaterialRenderState, cullMode, "CullMode")
SPARK_REFLECT_FIELD(MaterialRenderState, depthTest, "DepthTest")
SPARK_REFLECT_FIELD(MaterialRenderState, depthWrite, "DepthWrite")
SPARK_REFLECT_FIELD(MaterialRenderState, castShadows, "CastShadows")
SPARK_REFLECT_FIELD(MaterialRenderState, receiveShadows, "ReceiveShadows")
SPARK_REFLECT_FIELD(MaterialRenderState, renderQueue, "RenderQueue")
SPARK_REFLECT_FIELD(MaterialRenderState, doubleSided, "DoubleSided")
SPARK_REFLECT_END(MaterialRenderState)

// ============================================================================
// PLATFORM-INDEPENDENT IMPLEMENTATIONS
// ============================================================================

std::vector<std::string> Material::GetShaderPermutation() const
{
    std::vector<std::string> defines;

    if (HasTexture(MaterialTextureType::Albedo))
        defines.push_back("HAS_ALBEDO_MAP");
    if (HasTexture(MaterialTextureType::Normal))
        defines.push_back("HAS_NORMAL_MAP");
    if (HasTexture(MaterialTextureType::Metallic))
        defines.push_back("HAS_METALLIC_MAP");
    if (HasTexture(MaterialTextureType::Roughness))
        defines.push_back("HAS_ROUGHNESS_MAP");
    if (HasTexture(MaterialTextureType::Occlusion))
        defines.push_back("HAS_OCCLUSION_MAP");
    if (HasTexture(MaterialTextureType::Emissive))
        defines.push_back("HAS_EMISSIVE_MAP");
    if (HasTexture(MaterialTextureType::Height))
        defines.push_back("HAS_HEIGHT_MAP");
    if (HasTexture(MaterialTextureType::DetailAlbedo))
        defines.push_back("HAS_DETAIL_ALBEDO_MAP");
    if (HasTexture(MaterialTextureType::DetailNormal))
        defines.push_back("HAS_DETAIL_NORMAL_MAP");
    if (HasTexture(MaterialTextureType::Subsurface))
        defines.push_back("HAS_SUBSURFACE_MAP");
    if (HasTexture(MaterialTextureType::Transmission))
        defines.push_back("HAS_TRANSMISSION_MAP");
    if (HasTexture(MaterialTextureType::Clearcoat))
        defines.push_back("HAS_CLEARCOAT_MAP");
    if (HasTexture(MaterialTextureType::ClearcoatRoughness))
        defines.push_back("HAS_CLEARCOAT_ROUGHNESS_MAP");
    if (HasTexture(MaterialTextureType::Anisotropy))
        defines.push_back("HAS_ANISOTROPY_MAP");

    switch (m_renderState.blendMode)
    {
    case BlendMode::AlphaTest:
        defines.push_back("ALPHA_TEST");
        break;
    case BlendMode::Transparent:
        defines.push_back("ALPHA_BLEND");
        break;
    case BlendMode::Additive:
        defines.push_back("BLEND_ADDITIVE");
        break;
    case BlendMode::Multiply:
        defines.push_back("BLEND_MULTIPLY");
        break;
    case BlendMode::Screen:
        defines.push_back("BLEND_SCREEN");
        break;
    default:
        break;
    }

    if (m_advancedProperties.subsurfaceEnabled)
        defines.push_back("ENABLE_SUBSURFACE");
    if (m_advancedProperties.clearcoatEnabled)
        defines.push_back("ENABLE_CLEARCOAT");
    if (m_advancedProperties.anisotropyEnabled)
        defines.push_back("ENABLE_ANISOTROPY");
    if (m_advancedProperties.transmissionEnabled)
        defines.push_back("ENABLE_TRANSMISSION");
    if (m_advancedProperties.sheenEnabled)
        defines.push_back("ENABLE_SHEEN");
    if (m_advancedProperties.iridescenceEnabled)
        defines.push_back("ENABLE_IRIDESCENCE");

    if (m_renderState.doubleSided)
        defines.push_back("DOUBLE_SIDED");

    if (!m_activeVariant.empty())
    {
        auto it = m_variants.find(m_activeVariant);
        if (it != m_variants.end())
        {
            for (const auto& define : it->second)
            {
                defines.push_back(define);
            }
        }
    }

    return defines;
}
