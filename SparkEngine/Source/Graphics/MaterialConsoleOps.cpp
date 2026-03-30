#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file MaterialConsoleOps.cpp
 * @brief Console commands for material inspection, listing, creation, deletion, and validation.
 *
 * Covers read-only queries (list, info, dump, validate, variants, texture types),
 * lifecycle operations (reload, create variant, export, import, garbage collect, clear cache).
 * Editing/property-modification commands live in MaterialConsoleEdit.cpp.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodecsdk.h>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// CONSOLE METHODS — Inspection, Listing, Creation, Deletion, Validation
// ============================================================================

MaterialSystem::MaterialMetrics MaterialSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string MaterialSystem::Console_ListMaterials() const
{
    std::stringstream ss;
    ss << "=== Loaded Materials ===\n";
    for (const auto& pair : m_materials)
    {
        auto& material = pair.second;
        ss << pair.first;
        if (material)
        {
            ss << " (" << material->GetName() << ")";
        }
        ss << "\n";
    }
    ss << "Total: " << m_materials.size() << " materials";
    return ss.str();
}

std::string MaterialSystem::Console_GetMaterialInfo(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial)
    {
        return material->GetDetailedInfo();
    }
    return "Material not found: " + materialName;
}

bool MaterialSystem::Console_ReloadMaterial(const std::string& materialName)
{
    auto it = m_materials.find(materialName);
    if (it != m_materials.end())
    {
        if (it->second->LoadFromFile(materialName, m_device))
        {
            Spark::SimpleConsole::GetInstance().LogSuccess("Reloaded material: " + materialName);
            return true;
        }
        else
        {
            Spark::SimpleConsole::GetInstance().LogError("Failed to reload material: " + materialName);
        }
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    }
    return false;
}

int MaterialSystem::Console_ReloadAllMaterials()
{
    return ReloadAllMaterials();
}

bool MaterialSystem::Console_CreateVariant(const std::string& materialName, const std::string& variantName,
                                           const std::vector<std::string>& defines)
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial)
    {
        material->CreateVariant(variantName, defines);
        Spark::SimpleConsole::GetInstance().LogSuccess("Created variant '" + variantName +
                                                       "' for material: " + materialName);
        return true;
    }
    Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    return false;
}

void MaterialSystem::Console_ClearCache()
{
    size_t textureCount = m_textureCache.size();
    size_t samplerCount = m_samplerCache.size();

    m_textureCache.clear();
    m_samplerCache.clear();

    Spark::SimpleConsole::GetInstance().LogSuccess("Cleared cache: " + std::to_string(textureCount) + " textures, " +
                                                   std::to_string(samplerCount) + " samplers");
}

void MaterialSystem::Console_GarbageCollect()
{
    // Remove unused materials (those with only one reference - the one in the map)
    auto it = m_materials.begin();
    int removedCount = 0;

    while (it != m_materials.end())
    {
        if (it->second.use_count() == 1)
        {
            it = m_materials.erase(it);
            removedCount++;
        }
        else
        {
            ++it;
        }
    }

    Spark::SimpleConsole::GetInstance().LogSuccess("Garbage collected " + std::to_string(removedCount) +
                                                   " unused materials");
}

int MaterialSystem::Console_ValidateMaterials()
{
    int validCount = 0;
    int invalidCount = 0;
    std::vector<std::string> invalidMaterials;
    std::vector<std::string> warningMaterials;

    Spark::SimpleConsole::GetInstance().LogInfo("Starting comprehensive material validation...");

    for (const auto& pair : m_materials)
    {
        const std::string& materialName = pair.first;
        const auto& material = pair.second;

        if (!material)
        {
            invalidCount++;
            invalidMaterials.push_back(materialName + " (null material)");
            continue;
        }

        bool isValid = true;
        std::vector<std::string> issues;

        // Validate PBR properties
        const PBRProperties& pbr = material->GetPBRProperties();

        if (pbr.metallicFactor < 0.0f || pbr.metallicFactor > 1.0f)
        {
            isValid = false;
            issues.push_back("Metallic factor out of range [0,1]: " + std::to_string(pbr.metallicFactor));
        }

        if (pbr.roughnessFactor < 0.0f || pbr.roughnessFactor > 1.0f)
        {
            isValid = false;
            issues.push_back("Roughness factor out of range [0,1]: " + std::to_string(pbr.roughnessFactor));
        }

        if (pbr.normalScale < 0.0f)
        {
            isValid = false;
            issues.push_back("Normal scale cannot be negative: " + std::to_string(pbr.normalScale));
        }

        if (pbr.occlusionStrength < 0.0f || pbr.occlusionStrength > 1.0f)
        {
            isValid = false;
            issues.push_back("Occlusion strength out of range [0,1]: " + std::to_string(pbr.occlusionStrength));
        }

        if (pbr.alphaCutoff < 0.0f || pbr.alphaCutoff > 1.0f)
        {
            isValid = false;
            issues.push_back("Alpha cutoff out of range [0,1]: " + std::to_string(pbr.alphaCutoff));
        }

        if (pbr.indexOfRefraction < 1.0f)
        {
            isValid = false;
            issues.push_back("Index of refraction cannot be less than 1.0: " + std::to_string(pbr.indexOfRefraction));
        }

        if (pbr.emissiveFactor < 0.0f)
        {
            isValid = false;
            issues.push_back("Emissive factor cannot be negative: " + std::to_string(pbr.emissiveFactor));
        }

        // Validate albedo color
        if (pbr.albedoColor.x < 0.0f || pbr.albedoColor.x > 1.0f || pbr.albedoColor.y < 0.0f ||
            pbr.albedoColor.y > 1.0f || pbr.albedoColor.z < 0.0f || pbr.albedoColor.z > 1.0f ||
            pbr.albedoColor.w < 0.0f || pbr.albedoColor.w > 1.0f)
        {
            isValid = false;
            issues.push_back("Albedo color components out of range [0,1]");
        }

        // Validate emissive color (can be > 1.0 for HDR)
        if (pbr.emissiveColor.x < 0.0f || pbr.emissiveColor.y < 0.0f || pbr.emissiveColor.z < 0.0f)
        {
            isValid = false;
            issues.push_back("Emissive color components cannot be negative");
        }

        // Check for suspicious values (warnings, not errors)
        std::vector<std::string> warnings;

        if (pbr.metallicFactor > 0.9f && pbr.roughnessFactor < 0.1f)
        {
            warnings.push_back("Very high metallic + very low roughness may look unnatural");
        }

        if (pbr.emissiveFactor > 10.0f)
        {
            warnings.push_back("Very high emissive factor: " + std::to_string(pbr.emissiveFactor));
        }

        if (pbr.indexOfRefraction > 3.0f)
        {
            warnings.push_back("Unusually high IOR: " + std::to_string(pbr.indexOfRefraction));
        }

        // Validate advanced properties
        const AdvancedProperties& advanced = material->GetAdvancedProperties();

        if (advanced.subsurfaceEnabled && advanced.subsurfaceRadius <= 0.0f)
        {
            isValid = false;
            issues.push_back("Subsurface radius must be positive when subsurface is enabled");
        }

        if (advanced.clearcoatEnabled)
        {
            if (advanced.clearcoatFactor < 0.0f || advanced.clearcoatFactor > 1.0f)
            {
                isValid = false;
                issues.push_back("Clearcoat factor out of range [0,1]: " + std::to_string(advanced.clearcoatFactor));
            }
            if (advanced.clearcoatRoughness < 0.0f || advanced.clearcoatRoughness > 1.0f)
            {
                isValid = false;
                issues.push_back("Clearcoat roughness out of range [0,1]: " +
                                 std::to_string(advanced.clearcoatRoughness));
            }
        }

        if (advanced.anisotropyEnabled && (advanced.anisotropyFactor < -1.0f || advanced.anisotropyFactor > 1.0f))
        {
            isValid = false;
            issues.push_back("Anisotropy factor out of range [-1,1]: " + std::to_string(advanced.anisotropyFactor));
        }

        if (advanced.transmissionEnabled && (advanced.transmissionFactor < 0.0f || advanced.transmissionFactor > 1.0f))
        {
            isValid = false;
            issues.push_back("Transmission factor out of range [0,1]: " + std::to_string(advanced.transmissionFactor));
        }

        // Validate render state
        const MaterialRenderState& renderState = material->GetRenderState();

        if (renderState.renderQueue < 0 || renderState.renderQueue > 5000)
        {
            warnings.push_back("Render queue outside normal range [0-5000]: " +
                               std::to_string(renderState.renderQueue));
        }

        // Validate texture consistency
        bool hasAlbedo = material->HasTexture(MaterialTextureType::Albedo);
        bool hasNormal = material->HasTexture(MaterialTextureType::Normal);
        bool hasMetallic = material->HasTexture(MaterialTextureType::Metallic);
        bool hasRoughness = material->HasTexture(MaterialTextureType::Roughness);

        if (!hasAlbedo)
        {
            warnings.push_back("No albedo texture - material will use only base color");
        }

        if (hasNormal && pbr.normalScale == 0.0f)
        {
            warnings.push_back("Normal texture present but normal scale is 0");
        }

        if ((hasMetallic || hasRoughness) && (!hasMetallic || !hasRoughness))
        {
            warnings.push_back("Only one of metallic/roughness textures present - consider using packed textures");
        }

        // Compile results
        if (isValid)
        {
            validCount++;
            if (!warnings.empty())
            {
                warningMaterials.push_back(materialName + " (" + std::to_string(warnings.size()) + " warnings)");
            }
        }
        else
        {
            invalidCount++;
            std::string issueList = materialName + ": ";
            for (size_t i = 0; i < issues.size(); ++i)
            {
                if (i > 0)
                    issueList += ", ";
                issueList += issues[i];
            }
            invalidMaterials.push_back(issueList);
        }

        // Log detailed issues for invalid materials
        if (!isValid)
        {
            Spark::SimpleConsole::GetInstance().LogError("Invalid material '" + materialName + "':");
            for (const auto& issue : issues)
            {
                Spark::SimpleConsole::GetInstance().LogError("  - " + issue);
            }
        }

        // Log warnings
        if (!warnings.empty())
        {
            for (const auto& warning : warnings)
            {
                Spark::SimpleConsole::GetInstance().LogWarning("Material '" + materialName + "': " + warning);
            }
        }
    }

    // Summary report
    std::stringstream report;
    report << "=== Material Validation Complete ===\n";
    report << "Valid materials: " << validCount << "\n";
    report << "Invalid materials: " << invalidCount << "\n";
    report << "Materials with warnings: " << warningMaterials.size() << "\n";
    report << "Total materials: " << (validCount + invalidCount) << "\n";

    if (!invalidMaterials.empty())
    {
        report << "\nInvalid materials:\n";
        for (const auto& invalid : invalidMaterials)
        {
            report << "  - " << invalid << "\n";
        }
    }

    if (!warningMaterials.empty())
    {
        report << "\nMaterials with warnings:\n";
        for (const auto& warning : warningMaterials)
        {
            report << "  - " << warning << "\n";
        }
    }

    if (invalidCount == 0)
    {
        Spark::SimpleConsole::GetInstance().LogSuccess(report.str());
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning(report.str());
    }

    return validCount;
}

std::string MaterialSystem::Console_DumpMaterialDetails(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial)
    {
        return "Material not found: " + materialName;
    }

    std::stringstream ss;
    ss << "=== DETAILED MATERIAL DUMP: " << materialName << " ===\n\n";

    // Basic info
    ss << "[BASIC INFO]\n";
    ss << "Name: " << material->GetName() << "\n";
    ss << "Active Variant: " << material->GetActiveVariant() << "\n\n";

    // PBR Properties
    const auto& pbr = material->GetPBRProperties();
    ss << "[PBR PROPERTIES]\n";
    ss << std::fixed << std::setprecision(6);
    ss << "Albedo Color: (" << pbr.albedoColor.x << ", " << pbr.albedoColor.y << ", " << pbr.albedoColor.z << ", "
       << pbr.albedoColor.w << ")\n";
    ss << "Metallic Factor: " << pbr.metallicFactor << "\n";
    ss << "Roughness Factor: " << pbr.roughnessFactor << "\n";
    ss << "Normal Scale: " << pbr.normalScale << "\n";
    ss << "Occlusion Strength: " << pbr.occlusionStrength << "\n";
    ss << "Emissive Color: (" << pbr.emissiveColor.x << ", " << pbr.emissiveColor.y << ", " << pbr.emissiveColor.z
       << ")\n";
    ss << "Emissive Factor: " << pbr.emissiveFactor << "\n";
    ss << "Alpha Cutoff: " << pbr.alphaCutoff << "\n";
    ss << "Index of Refraction: " << pbr.indexOfRefraction << "\n\n";

    // Advanced Properties
    const auto& advanced = material->GetAdvancedProperties();
    ss << "[ADVANCED PROPERTIES]\n";
    ss << "Subsurface: " << (advanced.subsurfaceEnabled ? "Enabled" : "Disabled");
    if (advanced.subsurfaceEnabled)
    {
        ss << " - Color: (" << advanced.subsurfaceColor.x << ", " << advanced.subsurfaceColor.y << ", "
           << advanced.subsurfaceColor.z << "), Radius: " << advanced.subsurfaceRadius;
    }
    ss << "\n";

    ss << "Clearcoat: " << (advanced.clearcoatEnabled ? "Enabled" : "Disabled");
    if (advanced.clearcoatEnabled)
    {
        ss << " - Factor: " << advanced.clearcoatFactor << ", Roughness: " << advanced.clearcoatRoughness;
    }
    ss << "\n";

    ss << "Anisotropy: " << (advanced.anisotropyEnabled ? "Enabled" : "Disabled");
    if (advanced.anisotropyEnabled)
    {
        ss << " - Factor: " << advanced.anisotropyFactor << ", Direction: (" << advanced.anisotropyDirection.x << ", "
           << advanced.anisotropyDirection.y << ")";
    }
    ss << "\n";

    ss << "Transmission: " << (advanced.transmissionEnabled ? "Enabled" : "Disabled");
    if (advanced.transmissionEnabled)
    {
        ss << " - Factor: " << advanced.transmissionFactor << ", Color: (" << advanced.transmissionColor.x << ", "
           << advanced.transmissionColor.y << ", " << advanced.transmissionColor.z << ")";
    }
    ss << "\n";

    ss << "Sheen: " << (advanced.sheenEnabled ? "Enabled" : "Disabled");
    if (advanced.sheenEnabled)
    {
        ss << " - Color: (" << advanced.sheenColor.x << ", " << advanced.sheenColor.y << ", " << advanced.sheenColor.z
           << "), Roughness: " << advanced.sheenRoughness;
    }
    ss << "\n";

    ss << "Iridescence: " << (advanced.iridescenceEnabled ? "Enabled" : "Disabled");
    if (advanced.iridescenceEnabled)
    {
        ss << " - Factor: " << advanced.iridescenceFactor << ", IOR: " << advanced.iridescenceIOR
           << ", Thickness: " << advanced.iridescenceThickness << "nm";
    }
    ss << "\n\n";

    // Render State
    const auto& renderState = material->GetRenderState();
    ss << "[RENDER STATE]\n";
    ss << "Blend Mode: " << static_cast<int>(renderState.blendMode) << "\n";
    ss << "Cull Mode: " << static_cast<int>(renderState.cullMode) << "\n";
    ss << "Depth Test: " << (renderState.depthTest ? "Enabled" : "Disabled") << "\n";
    ss << "Depth Write: " << (renderState.depthWrite ? "Enabled" : "Disabled") << "\n";
    ss << "Cast Shadows: " << (renderState.castShadows ? "Enabled" : "Disabled") << "\n";
    ss << "Receive Shadows: " << (renderState.receiveShadows ? "Enabled" : "Disabled") << "\n";
    ss << "Render Queue: " << renderState.renderQueue << "\n";
    ss << "Double Sided: " << (renderState.doubleSided ? "Enabled" : "Disabled") << "\n\n";

    // Textures
    ss << "[TEXTURES]\n";
    for (int i = static_cast<int>(MaterialTextureType::Albedo); i <= static_cast<int>(MaterialTextureType::Custom3);
         ++i)
    {

        MaterialTextureType type = static_cast<MaterialTextureType>(i);
        if (material->HasTexture(type))
        {
            const auto& texture = material->GetTexture(type);
            ss << TextureTypeToString(type) << ": " << texture.filePath;
            if (!texture.enabled)
                ss << " (DISABLED)";
            ss << "\n  - Intensity: " << texture.intensity;
            ss << ", Tiling: (" << texture.tiling.x << ", " << texture.tiling.y << ")";
            ss << ", Offset: (" << texture.offset.x << ", " << texture.offset.y << ")\n";
        }
    }

    return ss.str();
}

bool MaterialSystem::Console_ExportMaterial(const std::string& materialName, const std::string& filePath)
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial)
    {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
        return false;
    }

    if (material->SaveToFile(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogSuccess("Exported material '" + materialName + "' to: " + filePath);
        return true;
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to export material: " + materialName);
        return false;
    }
}

bool MaterialSystem::Console_ImportMaterial(const std::string& filePath)
{
    if (!std::filesystem::exists(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogError("File not found: " + filePath);
        return false;
    }

    auto material = LoadMaterial(filePath);
    if (material && material != m_errorMaterial)
    {
        Spark::SimpleConsole::GetInstance().LogSuccess("Imported material from: " + filePath);
        return true;
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to import material from: " + filePath);
        return false;
    }
}

std::string MaterialSystem::Console_ListTextureTypes() const
{
    std::stringstream ss;
    ss << "=== Available Texture Types ===\n";

    for (int i = static_cast<int>(MaterialTextureType::Albedo); i <= static_cast<int>(MaterialTextureType::Custom3);
         ++i)
    {

        MaterialTextureType type = static_cast<MaterialTextureType>(i);
        ss << i << ": " << TextureTypeToString(type) << "\n";
    }

    return ss.str();
}

std::string MaterialSystem::Console_ListMaterialVariants(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial)
    {
        return "Material not found: " + materialName;
    }

    std::stringstream ss;
    ss << "=== Material Variants for '" << materialName << "' ===\n";

    auto variants = material->GetAvailableVariants();
    if (variants.empty())
    {
        ss << "No variants defined for this material.\n";
    }
    else
    {
        ss << "Available variants (" << variants.size() << "):\n";
        for (const auto& variant : variants)
        {
            ss << "  - " << variant;
            if (variant == material->GetActiveVariant())
            {
                ss << " (ACTIVE)";
            }
            ss << "\n";
        }
    }

    return ss.str();
}


#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "RHI/RHI.h"
#include "../Utils/Hash.h"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <filesystem>

// ============================================================================
// Console Methods — Linux: Inspection, Listing, Creation, Deletion, Validation
// ============================================================================

MaterialSystem::MaterialMetrics MaterialSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    MaterialMetrics metrics = m_metrics;
    metrics.loadedMaterials = static_cast<int>(m_materials.size());
    metrics.textureCount = static_cast<int>(m_textureCache.size());
    metrics.hotReloadEnabled = m_hotReloadEnabled;

    // Count total variants across all materials
    int totalVariants = 0;
    for (const auto& pair : m_materials)
    {
        if (pair.second)
        {
            totalVariants += static_cast<int>(pair.second->GetAvailableVariants().size());
        }
    }
    metrics.variantCount = totalVariants;

    return metrics;
}

std::string MaterialSystem::Console_ListMaterials() const
{
    std::stringstream ss;
    ss << "=== Loaded Materials (" << m_materials.size() << ") ===\n";
    if (m_materials.empty())
    {
        ss << "  (none)\n";
    }
    else
    {
        int index = 0;
        for (const auto& pair : m_materials)
        {
            ss << "  [" << index++ << "] " << pair.first;
            if (pair.second)
            {
                auto variants = pair.second->GetAvailableVariants();
                if (!variants.empty())
                {
                    ss << " (" << variants.size() << " variants)";
                }
            }
            ss << "\n";
        }
    }
    ss << "\nDefault material: " << (m_defaultMaterial ? m_defaultMaterial->GetName() : "(null)") << "\n";
    ss << "Error material:   " << (m_errorMaterial ? m_errorMaterial->GetName() : "(null)") << "\n";
    return ss.str();
}

std::string MaterialSystem::Console_GetMaterialInfo(const std::string& materialName) const
{
    auto mat = GetMaterial(materialName);
    if (!mat)
        return "Material not found: " + materialName;
    return mat->GetDetailedInfo();
}

bool MaterialSystem::Console_ReloadMaterial(const std::string& materialName)
{
    auto mat = GetMaterial(materialName);
    if (!mat)
    {
        fprintf(stderr, "[MaterialSystem] Cannot reload: material '%s' not found\n", materialName.c_str());
        return false;
    }
    // On Linux, no GPU resources to reload
    fprintf(stderr, "[MaterialSystem] Material '%s' marked for reload (no-op on Linux)\n", materialName.c_str());
    return true;
}

int MaterialSystem::Console_ReloadAllMaterials()
{
    return ReloadAllMaterials();
}

bool MaterialSystem::Console_CreateVariant(const std::string& materialName, const std::string& variantName,
                                           const std::vector<std::string>& defines)
{
    auto mat = GetMaterial(materialName);
    if (!mat)
    {
        fprintf(stderr, "[MaterialSystem] Cannot create variant: material '%s' not found\n", materialName.c_str());
        return false;
    }
    mat->CreateVariant(variantName, defines);
    return true;
}

void MaterialSystem::Console_ClearCache()
{
    size_t texCount = m_textureCache.size();
    size_t sampCount = m_samplerCache.size();
    m_textureCache.clear();
    m_samplerCache.clear();
    fprintf(stderr, "[MaterialSystem] Cache cleared: %zu textures, %zu samplers removed\n", texCount, sampCount);
    UpdateMetrics();
}

void MaterialSystem::Console_GarbageCollect()
{
    int collected = 0;
    for (auto it = m_materials.begin(); it != m_materials.end();)
    {
        if (it->second.use_count() == 1)
        {
            it = m_materials.erase(it);
            ++collected;
        }
        else
        {
            ++it;
        }
    }
    fprintf(stderr, "[MaterialSystem] Garbage collection complete: %d materials collected\n", collected);
    UpdateMetrics();
}

int MaterialSystem::Console_ValidateMaterials()
{
    int errors = 0;
    for (const auto& pair : m_materials)
    {
        if (!pair.second)
        {
            fprintf(stderr, "[MaterialSystem] Validation error: null material entry '%s'\n", pair.first.c_str());
            ++errors;
            continue;
        }

        // Check for invalid PBR values
        const auto& pbr = pair.second->GetPBRProperties();
        if (pbr.metallicFactor < 0.0f || pbr.metallicFactor > 1.0f)
        {
            fprintf(stderr, "[MaterialSystem] Validation warning: '%s' metallic out of range: %f\n", pair.first.c_str(),
                    pbr.metallicFactor);
            ++errors;
        }
        if (pbr.roughnessFactor < 0.0f || pbr.roughnessFactor > 1.0f)
        {
            fprintf(stderr, "[MaterialSystem] Validation warning: '%s' roughness out of range: %f\n",
                    pair.first.c_str(), pbr.roughnessFactor);
            ++errors;
        }
    }
    return errors;
}

std::string MaterialSystem::Console_DumpMaterialDetails(const std::string& materialName) const
{
    auto mat = GetMaterial(materialName);
    if (!mat)
        return "Material not found: " + materialName;

    std::stringstream ss;
    ss << mat->GetDetailedInfo();
    ss << "\n--- System Info ---\n";
    ss << "  Ref count:     " << m_materials.at(materialName).use_count() << "\n";
    ss << "  Hot reload:    " << (m_hotReloadEnabled ? "enabled" : "disabled") << "\n";
    ss << "  Platform:      Linux (CPU-side only)\n";
    return ss.str();
}

bool MaterialSystem::Console_ExportMaterial(const std::string& materialName, const std::string& filePath)
{
    auto mat = GetMaterial(materialName);
    if (!mat)
    {
        fprintf(stderr, "[MaterialSystem] Cannot export: material '%s' not found\n", materialName.c_str());
        return false;
    }
    bool result = mat->SaveToFile(filePath);
    if (result)
    {
        fprintf(stderr, "[MaterialSystem] Exported material '%s' to '%s'\n", materialName.c_str(), filePath.c_str());
    }
    return result;
}

bool MaterialSystem::Console_ImportMaterial(const std::string& filePath)
{
    auto mat = LoadMaterial(filePath);
    return mat != nullptr;
}

std::string MaterialSystem::Console_ListTextureTypes() const
{
    std::stringstream ss;
    ss << "Available texture types:\n";
    ss << "  Albedo, Normal, Metallic, Roughness, Occlusion, Emissive, Height,\n";
    ss << "  DetailAlbedo, DetailNormal, Subsurface, Transmission, Clearcoat,\n";
    ss << "  ClearcoatRoughness, Anisotropy, Custom0, Custom1, Custom2, Custom3";
    return ss.str();
}

std::string MaterialSystem::Console_ListMaterialVariants(const std::string& materialName) const
{
    auto mat = GetMaterial(materialName);
    if (!mat)
        return "Material not found: " + materialName;

    std::stringstream ss;
    ss << "=== Variants for " << materialName << " ===\n";
    auto variants = mat->GetAvailableVariants();
    if (variants.empty())
    {
        ss << "  (no variants defined)\n";
    }
    else
    {
        for (const auto& variant : variants)
        {
            ss << "  " << variant;
            if (variant == mat->GetActiveVariant())
                ss << " (ACTIVE)";
            ss << "\n";
        }
    }
    return ss.str();
}


#endif // SPARK_PLATFORM_WINDOWS
