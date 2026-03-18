#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PBRMaterial.cpp
 * @brief Core PBR material state management
 *
 * Contains constructor/destructor, property getters/setters, variant management,
 * console property helpers, material instancing, and detailed info.
 * Shader binding is in PBRMaterialBinding.cpp.
 * Serialization, reload, and shader permutation logic is in PBRMaterialLighting.cpp.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include <algorithm>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// MATERIAL CLASS IMPLEMENTATION — Core State
// ============================================================================

Material::Material(const std::string& name) : m_name(name)
{
    m_pbrProperties = {};
    m_advancedProperties = {};
    m_renderState = {};
    m_variants = {};
    m_activeVariant = "";
}

const MaterialTexture& Material::GetTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        return it->second;
    }

    Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " +
                                                   std::to_string(static_cast<int>(type)));

    // Return a default empty texture if not found
    static MaterialTexture emptyTexture;
    return emptyTexture;
}

void Material::SetTexture(MaterialTextureType type, const MaterialTexture& texture)
{
    m_textures[type] = texture;
}

bool Material::HasTexture(MaterialTextureType type) const
{
    return m_textures.find(type) != m_textures.end();
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end())
    {
        m_activeVariant = variantName;
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have variant '" +
                                                       variantName + "'");
    }
}

const std::string& Material::GetActiveVariant() const
{
    return m_activeVariant;
}

std::vector<std::string> Material::GetAvailableVariants() const
{
    std::vector<std::string> variants;
    for (const auto& pair : m_variants)
    {
        variants.push_back(pair.first);
    }
    return variants;
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "Material: " << m_name << "\n";
    ss << "Albedo: (" << m_pbrProperties.albedoColor.x << ", " << m_pbrProperties.albedoColor.y << ", "
       << m_pbrProperties.albedoColor.z << ")\n";
    ss << "Metallic: " << m_pbrProperties.metallicFactor << "\n";
    ss << "Roughness: " << m_pbrProperties.roughnessFactor << "\n";
    ss << "Normal Scale: " << m_pbrProperties.normalScale << "\n";
    ss << "Occlusion Strength: " << m_pbrProperties.occlusionStrength << "\n";
    ss << "Emissive: (" << m_pbrProperties.emissiveColor.x << ", " << m_pbrProperties.emissiveColor.y << ", "
       << m_pbrProperties.emissiveColor.z << ")\n";
    ss << "Emissive Factor: " << m_pbrProperties.emissiveFactor << "\n";
    ss << "Alpha Cutoff: " << m_pbrProperties.alphaCutoff << "\n";
    ss << "IOR: " << m_pbrProperties.indexOfRefraction << "\n";
    ss << "Blend Mode: " << static_cast<int>(m_renderState.blendMode) << "\n";
    ss << "Cull Mode: " << static_cast<int>(m_renderState.cullMode) << "\n";
    ss << "Depth Test: " << (m_renderState.depthTest ? "Yes" : "No") << "\n";
    ss << "Depth Write: " << (m_renderState.depthWrite ? "Yes" : "No") << "\n";
    ss << "Cast Shadows: " << (m_renderState.castShadows ? "Yes" : "No") << "\n";
    ss << "Receive Shadows: " << (m_renderState.receiveShadows ? "Yes" : "No") << "\n";
    ss << "Textures: " << m_textures.size() << "\n";

    // List textures
    for (const auto& pair : m_textures)
    {
        if (pair.second.enabled)
        {
            ss << "  - Type" << static_cast<int>(pair.first) << ": " << pair.second.filePath << "\n";
        }
    }

    ss << "Variants: " << m_variants.size() << "\n";
    if (!m_activeVariant.empty())
    {
        ss << "Active Variant: " << m_activeVariant << "\n";
    }

    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic")
    {
        m_pbrProperties.metallicFactor = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "roughness")
    {
        m_pbrProperties.roughnessFactor = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "normal")
    {
        m_pbrProperties.normalScale = std::max(0.0f, value);
    }
    else if (property == "occlusion")
    {
        m_pbrProperties.occlusionStrength = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "emissive_factor")
    {
        m_pbrProperties.emissiveFactor = std::max(0.0f, value);
    }
    else if (property == "alpha_cutoff")
    {
        m_pbrProperties.alphaCutoff = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "ior")
    {
        m_pbrProperties.indexOfRefraction = std::max(1.0f, value);
    }
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo")
    {
        m_pbrProperties.albedoColor = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f),
                                       m_pbrProperties.albedoColor.w};
    }
    else if (property == "emissive")
    {
        m_pbrProperties.emissiveColor = {std::max(0.0f, r), std::max(0.0f, g), std::max(0.0f, b)};
    }
}

std::shared_ptr<Material> Material::CreateInstance(const std::string& instanceName) const
{
    auto instance = std::make_shared<Material>(instanceName);
    instance->m_pbrProperties = m_pbrProperties;
    instance->m_advancedProperties = m_advancedProperties;
    instance->m_renderState = m_renderState;
    instance->m_textures = m_textures;
    instance->m_variants = m_variants;
    instance->m_activeVariant = m_activeVariant;

    // Share compiled pipeline states from the template (read-only, safe to share)
    instance->m_blendState = m_blendState;
    instance->m_depthStencilState = m_depthStencilState;
    instance->m_rasterizerState = m_rasterizerState;
    // Constant buffer is NOT shared; the instance gets its own so properties can diverge
    instance->m_compiled = false;

    Spark::SimpleConsole::GetInstance().LogInfo("Created material instance '" + instanceName + "' from template '" +
                                                m_name + "'");
    return instance;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <cmath>

// ============================================================================
// Material (Linux) — Core State
// ============================================================================

Material::Material(const std::string& name) : m_name(name)
{
    // Initialize PBR defaults: white albedo, dielectric, medium roughness
    m_pbrProperties.albedoColor = {1.0f, 1.0f, 1.0f, 1.0f};
    m_pbrProperties.metallicFactor = 0.0f;
    m_pbrProperties.roughnessFactor = 0.5f;
    m_pbrProperties.normalScale = 1.0f;
    m_pbrProperties.occlusionStrength = 1.0f;
    m_pbrProperties.emissiveColor = {0.0f, 0.0f, 0.0f};
    m_pbrProperties.emissiveFactor = 0.0f;
    m_pbrProperties.alphaCutoff = 0.5f;
    m_pbrProperties.indexOfRefraction = 1.5f;

    m_advancedProperties = {};
    m_renderState = {};
    m_activeVariant = "";
}

const MaterialTexture& Material::GetTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        return it->second;
    }
    static MaterialTexture defaultTexture;
    return defaultTexture;
}

const std::string& Material::GetActiveVariant() const
{
    return m_activeVariant;
}

std::vector<std::string> Material::GetAvailableVariants() const
{
    std::vector<std::string> variants;
    variants.reserve(m_variants.size());
    for (const auto& pair : m_variants)
    {
        variants.push_back(pair.first);
    }
    return variants;
}

void Material::SetTexture(MaterialTextureType type, const MaterialTexture& texture)
{
    m_textures[type] = texture;
}

bool Material::HasTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    return it != m_textures.end() && it->second.enabled;
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end())
    {
        m_activeVariant = variantName;
    }
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "=== Material: " << m_name << " ===\n";

    // PBR Properties
    ss << "\n--- PBR Properties ---\n";
    ss << "  Albedo:      (" << m_pbrProperties.albedoColor.x << ", " << m_pbrProperties.albedoColor.y << ", "
       << m_pbrProperties.albedoColor.z << ", " << m_pbrProperties.albedoColor.w << ")\n";
    ss << "  Metallic:    " << m_pbrProperties.metallicFactor << "\n";
    ss << "  Roughness:   " << m_pbrProperties.roughnessFactor << "\n";
    ss << "  Normal Scale:" << m_pbrProperties.normalScale << "\n";
    ss << "  Occlusion:   " << m_pbrProperties.occlusionStrength << "\n";
    ss << "  Emissive:    (" << m_pbrProperties.emissiveColor.x << ", " << m_pbrProperties.emissiveColor.y << ", "
       << m_pbrProperties.emissiveColor.z << ") x " << m_pbrProperties.emissiveFactor << "\n";
    ss << "  Alpha Cutoff:" << m_pbrProperties.alphaCutoff << "\n";
    ss << "  IOR:         " << m_pbrProperties.indexOfRefraction << "\n";

    // Advanced Properties
    ss << "\n--- Advanced Properties ---\n";
    ss << "  Subsurface:    " << (m_advancedProperties.subsurfaceEnabled ? "ON" : "OFF");
    if (m_advancedProperties.subsurfaceEnabled)
    {
        ss << " (radius=" << m_advancedProperties.subsurfaceRadius << ")";
    }
    ss << "\n";
    ss << "  Clearcoat:     " << (m_advancedProperties.clearcoatEnabled ? "ON" : "OFF");
    if (m_advancedProperties.clearcoatEnabled)
    {
        ss << " (factor=" << m_advancedProperties.clearcoatFactor
           << ", roughness=" << m_advancedProperties.clearcoatRoughness << ")";
    }
    ss << "\n";
    ss << "  Anisotropy:    " << (m_advancedProperties.anisotropyEnabled ? "ON" : "OFF");
    if (m_advancedProperties.anisotropyEnabled)
    {
        ss << " (factor=" << m_advancedProperties.anisotropyFactor << ")";
    }
    ss << "\n";
    ss << "  Transmission:  " << (m_advancedProperties.transmissionEnabled ? "ON" : "OFF");
    if (m_advancedProperties.transmissionEnabled)
    {
        ss << " (factor=" << m_advancedProperties.transmissionFactor << ")";
    }
    ss << "\n";
    ss << "  Sheen:         " << (m_advancedProperties.sheenEnabled ? "ON" : "OFF") << "\n";
    ss << "  Iridescence:   " << (m_advancedProperties.iridescenceEnabled ? "ON" : "OFF") << "\n";

    // Render State
    ss << "\n--- Render State ---\n";
    const char* blendNames[] = {"Opaque", "AlphaTest", "Transparent", "Additive", "Multiply", "Screen"};
    const char* cullNames[] = {"None", "Front", "Back"};
    ss << "  Blend Mode:    " << blendNames[static_cast<int>(m_renderState.blendMode)] << "\n";
    ss << "  Cull Mode:     " << cullNames[static_cast<int>(m_renderState.cullMode)] << "\n";
    ss << "  Depth Test:    " << (m_renderState.depthTest ? "ON" : "OFF") << "\n";
    ss << "  Depth Write:   " << (m_renderState.depthWrite ? "ON" : "OFF") << "\n";
    ss << "  Cast Shadows:  " << (m_renderState.castShadows ? "ON" : "OFF") << "\n";
    ss << "  Recv Shadows:  " << (m_renderState.receiveShadows ? "ON" : "OFF") << "\n";
    ss << "  Render Queue:  " << m_renderState.renderQueue << "\n";
    ss << "  Double Sided:  " << (m_renderState.doubleSided ? "ON" : "OFF") << "\n";

    // Textures
    ss << "\n--- Textures (" << m_textures.size() << " slots) ---\n";
    for (const auto& pair : m_textures)
    {
        ss << "  [" << static_cast<int>(pair.first) << "] " << (pair.second.enabled ? "ACTIVE" : "INACTIVE")
           << " path=\"" << pair.second.filePath << "\"" << " intensity=" << pair.second.intensity << "\n";
    }

    // Variants
    ss << "\n--- Variants (" << m_variants.size() << ") ---\n";
    for (const auto& pair : m_variants)
    {
        ss << "  " << pair.first;
        if (pair.first == m_activeVariant)
            ss << " (ACTIVE)";
        ss << " [";
        for (size_t i = 0; i < pair.second.size(); ++i)
        {
            if (i > 0)
                ss << ", ";
            ss << pair.second[i];
        }
        ss << "]\n";
    }

    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic")
        m_pbrProperties.metallicFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "roughness")
        m_pbrProperties.roughnessFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "normalscale")
        m_pbrProperties.normalScale = value;
    else if (property == "occlusion")
        m_pbrProperties.occlusionStrength = std::clamp(value, 0.0f, 1.0f);
    else if (property == "emissive")
        m_pbrProperties.emissiveFactor = std::fmax(value, 0.0f);
    else if (property == "alphacutoff")
        m_pbrProperties.alphaCutoff = std::clamp(value, 0.0f, 1.0f);
    else if (property == "ior")
        m_pbrProperties.indexOfRefraction = std::fmax(value, 1.0f);
    else if (property == "clearcoat")
        m_advancedProperties.clearcoatFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "clearcoatroughness")
        m_advancedProperties.clearcoatRoughness = std::clamp(value, 0.0f, 1.0f);
    else if (property == "anisotropy")
        m_advancedProperties.anisotropyFactor = std::clamp(value, -1.0f, 1.0f);
    else if (property == "transmission")
        m_advancedProperties.transmissionFactor = std::clamp(value, 0.0f, 1.0f);
    else
    {
        fprintf(stderr, "[Material] Unknown property: '%s'\n", property.c_str());
    }
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo")
    {
        m_pbrProperties.albedoColor = {r, g, b, m_pbrProperties.albedoColor.w};
    }
    else if (property == "emissive")
    {
        m_pbrProperties.emissiveColor = {r, g, b};
    }
    else if (property == "subsurface")
    {
        m_advancedProperties.subsurfaceColor = {r, g, b};
    }
    else if (property == "transmission")
    {
        m_advancedProperties.transmissionColor = {r, g, b};
    }
    else if (property == "sheen")
    {
        m_advancedProperties.sheenColor = {r, g, b};
    }
    else
    {
        fprintf(stderr, "[Material] Unknown color property: '%s'\n", property.c_str());
    }
}

std::shared_ptr<Material> Material::CreateInstance(const std::string& instanceName) const
{
    auto instance = std::make_shared<Material>(instanceName);
    instance->m_pbrProperties = m_pbrProperties;
    instance->m_advancedProperties = m_advancedProperties;
    instance->m_renderState = m_renderState;
    instance->m_textures = m_textures;
    instance->m_variants = m_variants;
    instance->m_activeVariant = m_activeVariant;
    instance->m_compiled = false;
    return instance;
}

#endif // SPARK_PLATFORM_WINDOWS
