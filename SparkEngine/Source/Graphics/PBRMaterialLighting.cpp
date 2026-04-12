/**
 * @file PBRMaterialLighting.cpp
 * @brief Platform-independent PBR material serialization and shader permutation logic
 *
 * Contains SaveToFile (INI-format material I/O) and GetShaderPermutation
 * (shader define generation based on material state). These are CPU-only
 * and run identically on all platforms.
 *
 * Platform-specific code:
 *   - PBRMaterialLightingWindows.cpp — LoadFromFile, ReloadMaterial (D3D11)
 *   - PBRMaterialLightingLinux.cpp   — LoadFromFile, ReloadMaterial (stubs)
 *
 * Core material state is in PBRMaterial.cpp.
 * Shader binding is in PBRMaterialBinding.cpp.
 */

#include "MaterialSystem.h"
#include "Utils/LocalFileCache.h"
#include "../Core/Reflection.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

// ============================================================================
// Reflection registrations for PBR material structs.
// field.name is the INI key (PascalCase) used in .material files.
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
// Generic INI section write from reflected fields.
// ============================================================================

namespace
{

    void WriteReflectedINISection(std::ofstream& file, const void* data, const Spark::TypeInfo& typeInfo)
    {
        for (const auto& field : typeInfo.fields)
        {
            file << field.name << "=" << Spark::GetFieldAsString(data, field) << "\n";
        }
    }

    bool LoadReflectedINIField(void* data, const Spark::TypeInfo& typeInfo, const std::string& key,
                               const std::string& value)
    {
        for (const auto& field : typeInfo.fields)
        {
            if (field.name == key)
            {
                Spark::SetFieldFromString(data, field, value);
                return true;
            }
        }
        return false;
    }

} // anonymous namespace

// ============================================================================
// PLATFORM-INDEPENDENT IMPLEMENTATIONS
// ============================================================================

bool Material::SaveToFile(const std::string& filePath) const
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Saving material '%s' to file: %s", m_name.c_str(), filePath.c_str());
    try
    {
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            Spark::SimpleConsole::GetInstance().LogError("Cannot open file for writing: " + filePath);
            return false;
        }

        if (m_name.empty())
        {
            Spark::SimpleConsole::GetInstance().LogError("Cannot save material with empty name");
            return false;
        }

        file << "# Spark Engine Material File\n";
        file << "# Version: 1.0\n";
        file << "# Generated: "
             << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << "\n";
        file << "\n";

        file << "[Material]\n";
        file << "Name=" << m_name << "\n";
        file << "ActiveVariant=" << m_activeVariant << "\n";
        file << "\n";

        file << "[PBR]\n";
        WriteReflectedINISection(file, &m_pbrProperties,
                                 *Spark::TypeRegistry::Get().FindType(GetTypeId<PBRProperties>()));
        file << "\n";

        file << "[Advanced]\n";
        file << "SubsurfaceEnabled=" << (m_advancedProperties.subsurfaceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.subsurfaceEnabled)
        {
            file << "SubsurfaceColor=" << m_advancedProperties.subsurfaceColor.x << ","
                 << m_advancedProperties.subsurfaceColor.y << "," << m_advancedProperties.subsurfaceColor.z << "\n";
            file << "SubsurfaceRadius=" << m_advancedProperties.subsurfaceRadius << "\n";
        }

        file << "ClearcoatEnabled=" << (m_advancedProperties.clearcoatEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.clearcoatEnabled)
        {
            file << "ClearcoatFactor=" << m_advancedProperties.clearcoatFactor << "\n";
            file << "ClearcoatRoughness=" << m_advancedProperties.clearcoatRoughness << "\n";
        }

        file << "AnisotropyEnabled=" << (m_advancedProperties.anisotropyEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.anisotropyEnabled)
        {
            file << "AnisotropyFactor=" << m_advancedProperties.anisotropyFactor << "\n";
            file << "AnisotropyDirection=" << m_advancedProperties.anisotropyDirection.x << ","
                 << m_advancedProperties.anisotropyDirection.y << "\n";
        }

        file << "TransmissionEnabled=" << (m_advancedProperties.transmissionEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.transmissionEnabled)
        {
            file << "TransmissionFactor=" << m_advancedProperties.transmissionFactor << "\n";
            file << "TransmissionColor=" << m_advancedProperties.transmissionColor.x << ","
                 << m_advancedProperties.transmissionColor.y << "," << m_advancedProperties.transmissionColor.z << "\n";
        }

        file << "SheenEnabled=" << (m_advancedProperties.sheenEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.sheenEnabled)
        {
            file << "SheenColor=" << m_advancedProperties.sheenColor.x << "," << m_advancedProperties.sheenColor.y
                 << "," << m_advancedProperties.sheenColor.z << "\n";
            file << "SheenRoughness=" << m_advancedProperties.sheenRoughness << "\n";
        }

        file << "IridescenceEnabled=" << (m_advancedProperties.iridescenceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.iridescenceEnabled)
        {
            file << "IridescenceFactor=" << m_advancedProperties.iridescenceFactor << "\n";
            file << "IridescenceIOR=" << m_advancedProperties.iridescenceIOR << "\n";
            file << "IridescenceThickness=" << m_advancedProperties.iridescenceThickness << "\n";
        }
        file << "\n";

        file << "[RenderState]\n";
        WriteReflectedINISection(file, &m_renderState,
                                 *Spark::TypeRegistry::Get().FindType(GetTypeId<MaterialRenderState>()));
        file << "\n";

        file << "[Textures]\n";
        for (const auto& pair : m_textures)
        {
            if (!pair.second.filePath.empty())
            {
                file << "Texture" << static_cast<int>(pair.first) << "=" << pair.second.filePath << "\n";
                file << "Texture" << static_cast<int>(pair.first)
                     << "_Enabled=" << (pair.second.enabled ? "true" : "false") << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Intensity=" << pair.second.intensity << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Tiling=" << pair.second.tiling.x << ","
                     << pair.second.tiling.y << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Offset=" << pair.second.offset.x << ","
                     << pair.second.offset.y << "\n";
            }
        }
        file << "\n";

        if (!m_variants.empty())
        {
            file << "[Variants]\n";
            for (const auto& variantPair : m_variants)
            {
                file << "Variant_" << variantPair.first << "=";
                for (size_t i = 0; i < variantPair.second.size(); ++i)
                {
                    if (i > 0)
                        file << ",";
                    file << variantPair.second[i];
                }
                file << "\n";
            }
            file << "\n";
        }

        file.close();

        if (m_fileCache)
        {
            m_fileCache->Invalidate(filePath);
        }

        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' saved to: " + filePath);
        return true;
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception while saving material '" + m_name +
                                                     "': " + std::string(e.what()));
        return false;
    }
}

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
