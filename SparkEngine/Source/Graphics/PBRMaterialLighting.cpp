#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PBRMaterialLighting.cpp
 * @brief PBR material serialization, reload, and shader permutation logic
 *
 * Contains SaveToFile, LoadFromFile (INI-format material I/O), ReloadMaterial
 * (full texture + pipeline re-creation), and GetShaderPermutation (platform-independent
 * shader define generation based on material state).
 * Core material state is in PBRMaterial.cpp.
 * Shader binding is in PBRMaterialBinding.cpp.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// MATERIAL CLASS — Serialization & Reload (Windows)
// ============================================================================

bool Material::SaveToFile(const std::string& filePath) const
{
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

        // Write file header with version for future compatibility
        file << "# Spark Engine Material File\n";
        file << "# Version: 1.0\n";
        file << "# Generated: "
             << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << "\n";
        file << "\n";

        // Write basic material info
        file << "[Material]\n";
        file << "Name=" << m_name << "\n";
        file << "ActiveVariant=" << m_activeVariant << "\n";
        file << "\n";

        // Write PBR properties with full precision
        file << "[PBR]\n";
        file << "AlbedoColor=" << m_pbrProperties.albedoColor.x << "," << m_pbrProperties.albedoColor.y << ","
             << m_pbrProperties.albedoColor.z << "," << m_pbrProperties.albedoColor.w << "\n";
        file << "MetallicFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.metallicFactor << "\n";
        file << "RoughnessFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.roughnessFactor << "\n";
        file << "NormalScale=" << std::fixed << std::setprecision(6) << m_pbrProperties.normalScale << "\n";
        file << "OcclusionStrength=" << std::fixed << std::setprecision(6) << m_pbrProperties.occlusionStrength << "\n";
        file << "EmissiveColor=" << m_pbrProperties.emissiveColor.x << "," << m_pbrProperties.emissiveColor.y << ","
             << m_pbrProperties.emissiveColor.z << "\n";
        file << "EmissiveFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.emissiveFactor << "\n";
        file << "AlphaCutoff=" << std::fixed << std::setprecision(6) << m_pbrProperties.alphaCutoff << "\n";
        file << "IndexOfRefraction=" << std::fixed << std::setprecision(6) << m_pbrProperties.indexOfRefraction << "\n";
        file << "\n";

        // Write advanced properties
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

        // Write render state
        file << "[RenderState]\n";
        file << "BlendMode=" << static_cast<int>(m_renderState.blendMode) << "\n";
        file << "CullMode=" << static_cast<int>(m_renderState.cullMode) << "\n";
        file << "DepthTest=" << (m_renderState.depthTest ? "true" : "false") << "\n";
        file << "DepthWrite=" << (m_renderState.depthWrite ? "true" : "false") << "\n";
        file << "CastShadows=" << (m_renderState.castShadows ? "true" : "false") << "\n";
        file << "ReceiveShadows=" << (m_renderState.receiveShadows ? "true" : "false") << "\n";
        file << "RenderQueue=" << m_renderState.renderQueue << "\n";
        file << "DoubleSided=" << (m_renderState.doubleSided ? "true" : "false") << "\n";
        file << "\n";

        // Write textures with full parameters
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

        // Write material variants
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

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* device)
{
    try
    {
        std::string fileContent;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(filePath);
            if (result.IsOk())
            {
                fileContent = result.Value();
            }
        }

        if (fileContent.empty())
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                Spark::SimpleConsole::GetInstance().LogError("Cannot open file for reading: " + filePath);
                return false;
            }
            fileContent.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        std::istringstream fileStream(fileContent);
        std::string line;
        std::string currentSection = "";
        int lineNumber = 0;

        // Helper lambda to safely parse a single float value
        auto safeStof = [](const std::string& value, float defaultVal = 0.0f) -> float
        {
            try
            {
                return std::stof(value);
            }
            catch (const std::exception&)
            {
                return defaultVal;
            }
        };

        // Helper lambda to parse comma-separated values
        auto parseFloatArray = [](const std::string& value, std::vector<float>& output)
        {
            output.clear();
            std::stringstream ss(value);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                try
                {
                    output.push_back(std::stof(item));
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }
            return true;
        };

        // Helper lambda to parse boolean values
        auto parseBool = [](const std::string& value) -> bool
        { return value == "true" || value == "1" || value == "yes"; };

        // Helper lambda to trim whitespace
        auto trim = [](const std::string& str) -> std::string
        {
            size_t start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            size_t end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        };

        while (std::getline(fileStream, line))
        {
            lineNumber++;
            line = trim(line);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            // Check for section headers
            if (line[0] == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            // Parse key=value pairs
            size_t equalPos = line.find('=');
            if (equalPos == std::string::npos)
            {
                Spark::SimpleConsole::GetInstance().LogWarning("Invalid line format at line " +
                                                               std::to_string(lineNumber) + " in: " + filePath);
                continue;
            }

            std::string key = trim(line.substr(0, equalPos));
            std::string value = trim(line.substr(equalPos + 1));

            try
            {
                // Parse based on current section
                if (currentSection == "Material")
                {
                    if (key == "Name")
                    {
                        m_name = value;
                    }
                    else if (key == "ActiveVariant")
                    {
                        m_activeVariant = value;
                    }
                }
                else if (currentSection == "PBR")
                {
                    if (key == "AlbedoColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_pbrProperties.albedoColor.x = components[0];
                            m_pbrProperties.albedoColor.y = components[1];
                            m_pbrProperties.albedoColor.z = components[2];
                            m_pbrProperties.albedoColor.w = components.size() > 3 ? components[3] : 1.0f;
                        }
                    }
                    else if (key == "MetallicFactor")
                    {
                        m_pbrProperties.metallicFactor = safeStof(value);
                    }
                    else if (key == "RoughnessFactor")
                    {
                        m_pbrProperties.roughnessFactor = safeStof(value);
                    }
                    else if (key == "NormalScale")
                    {
                        m_pbrProperties.normalScale = safeStof(value);
                    }
                    else if (key == "OcclusionStrength")
                    {
                        m_pbrProperties.occlusionStrength = safeStof(value);
                    }
                    else if (key == "EmissiveColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_pbrProperties.emissiveColor.x = components[0];
                            m_pbrProperties.emissiveColor.y = components[1];
                            m_pbrProperties.emissiveColor.z = components[2];
                        }
                    }
                    else if (key == "EmissiveFactor")
                    {
                        m_pbrProperties.emissiveFactor = safeStof(value);
                    }
                    else if (key == "AlphaCutoff")
                    {
                        m_pbrProperties.alphaCutoff = safeStof(value);
                    }
                    else if (key == "IndexOfRefraction")
                    {
                        m_pbrProperties.indexOfRefraction = safeStof(value);
                    }
                }
                else if (currentSection == "Advanced")
                {
                    if (key == "SubsurfaceEnabled")
                    {
                        m_advancedProperties.subsurfaceEnabled = parseBool(value);
                    }
                    else if (key == "SubsurfaceColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.subsurfaceColor.x = components[0];
                            m_advancedProperties.subsurfaceColor.y = components[1];
                            m_advancedProperties.subsurfaceColor.z = components[2];
                        }
                    }
                    else if (key == "SubsurfaceRadius")
                    {
                        m_advancedProperties.subsurfaceRadius = safeStof(value);
                    }
                    else if (key == "ClearcoatEnabled")
                    {
                        m_advancedProperties.clearcoatEnabled = parseBool(value);
                    }
                    else if (key == "ClearcoatFactor")
                    {
                        m_advancedProperties.clearcoatFactor = safeStof(value);
                    }
                    else if (key == "ClearcoatRoughness")
                    {
                        m_advancedProperties.clearcoatRoughness = safeStof(value);
                    }
                    else if (key == "AnisotropyEnabled")
                    {
                        m_advancedProperties.anisotropyEnabled = parseBool(value);
                    }
                    else if (key == "AnisotropyFactor")
                    {
                        m_advancedProperties.anisotropyFactor = safeStof(value);
                    }
                    else if (key == "AnisotropyDirection")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 2)
                        {
                            m_advancedProperties.anisotropyDirection.x = components[0];
                            m_advancedProperties.anisotropyDirection.y = components[1];
                        }
                    }
                    else if (key == "TransmissionEnabled")
                    {
                        m_advancedProperties.transmissionEnabled = parseBool(value);
                    }
                    else if (key == "TransmissionFactor")
                    {
                        m_advancedProperties.transmissionFactor = safeStof(value);
                    }
                    else if (key == "TransmissionColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.transmissionColor.x = components[0];
                            m_advancedProperties.transmissionColor.y = components[1];
                            m_advancedProperties.transmissionColor.z = components[2];
                        }
                    }
                    else if (key == "SheenEnabled")
                    {
                        m_advancedProperties.sheenEnabled = parseBool(value);
                    }
                    else if (key == "SheenColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.sheenColor.x = components[0];
                            m_advancedProperties.sheenColor.y = components[1];
                            m_advancedProperties.sheenColor.z = components[2];
                        }
                    }
                    else if (key == "SheenRoughness")
                    {
                        m_advancedProperties.sheenRoughness = safeStof(value);
                    }
                    else if (key == "IridescenceEnabled")
                    {
                        m_advancedProperties.iridescenceEnabled = parseBool(value);
                    }
                    else if (key == "IridescenceFactor")
                    {
                        m_advancedProperties.iridescenceFactor = safeStof(value);
                    }
                    else if (key == "IridescenceIOR")
                    {
                        m_advancedProperties.iridescenceIOR = safeStof(value);
                    }
                    else if (key == "IridescenceThickness")
                    {
                        m_advancedProperties.iridescenceThickness = safeStof(value);
                    }
                }
                else if (currentSection == "RenderState")
                {
                    if (key == "BlendMode")
                    {
                        m_renderState.blendMode = static_cast<BlendMode>(std::stoi(value));
                    }
                    else if (key == "CullMode")
                    {
                        m_renderState.cullMode = static_cast<CullMode>(std::stoi(value));
                    }
                    else if (key == "DepthTest")
                    {
                        m_renderState.depthTest = parseBool(value);
                    }
                    else if (key == "DepthWrite")
                    {
                        m_renderState.depthWrite = parseBool(value);
                    }
                    else if (key == "CastShadows")
                    {
                        m_renderState.castShadows = parseBool(value);
                    }
                    else if (key == "ReceiveShadows")
                    {
                        m_renderState.receiveShadows = parseBool(value);
                    }
                    else if (key == "RenderQueue")
                    {
                        m_renderState.renderQueue = std::stoi(value);
                    }
                    else if (key == "DoubleSided")
                    {
                        m_renderState.doubleSided = parseBool(value);
                    }
                }
                else if (currentSection == "Textures")
                {
                    // Parse texture entries
                    if (key.substr(0, 7) == "Texture" && !key.contains("_"))
                    {
                        // Main texture path
                        int textureType = std::stoi(key.substr(7));
                        LoadTexture(static_cast<MaterialTextureType>(textureType), value, device);
                    }
                    // Parse texture properties (enabled, intensity, tiling, offset)
                    else if (key.contains("_Enabled"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Enabled"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                it->second.enabled = parseBool(value);
                            }
                        }
                    }
                    else if (key.contains("_Intensity"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Intensity"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                it->second.intensity = safeStof(value);
                            }
                        }
                    }
                    else if (key.contains("_Tiling"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Tiling"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2)
                                {
                                    it->second.tiling.x = components[0];
                                    it->second.tiling.y = components[1];
                                }
                            }
                        }
                    }
                    else if (key.contains("_Offset"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Offset"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2)
                                {
                                    it->second.offset.x = components[0];
                                    it->second.offset.y = components[1];
                                }
                            }
                        }
                    }
                }
                else if (currentSection == "Variants")
                {
                    if (key.substr(0, 8) == "Variant_")
                    {
                        std::string variantName = key.substr(8);
                        std::vector<std::string> defines;
                        std::stringstream ss(value);
                        std::string define;
                        while (std::getline(ss, define, ','))
                        {
                            defines.push_back(trim(define));
                        }
                        m_variants[variantName] = defines;
                    }
                }
            }
            catch (const std::exception& e)
            {
                Spark::SimpleConsole::GetInstance().LogError("Error parsing line " + std::to_string(lineNumber) +
                                                             " in " + filePath + ": " + std::string(e.what()));
                continue;
            }
        }

        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' loaded from: " + filePath +
                                                       " (textures: " + std::to_string(m_textures.size()) +
                                                       ", variants: " + std::to_string(m_variants.size()) + ")");

        return true;
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception while loading material from " + filePath + ": " +
                                                     std::string(e.what()));
        return false;
    }
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
    if (!device)
    {
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: device is null for material '" + m_name + "'");
        return false;
    }

    bool allSucceeded = true;

    // Collect texture entries to reload (cannot erase while iterating)
    std::vector<std::pair<MaterialTextureType, std::string>> texturesToReload;
    for (const auto& [type, matTexture] : m_textures)
    {
        if (!matTexture.filePath.empty())
        {
            texturesToReload.emplace_back(type, matTexture.filePath);
        }
    }

    // Clear all existing textures, then reload from disk
    m_textures.clear();

    for (const auto& [type, filePath] : texturesToReload)
    {
        if (!std::filesystem::exists(filePath))
        {
            Spark::SimpleConsole::GetInstance().LogWarning("ReloadMaterial: texture file missing for '" + m_name +
                                                           "': " + filePath);
            allSucceeded = false;
            continue;
        }

        if (!LoadTexture(type, filePath, device))
        {
            Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: failed to reload texture '" + filePath +
                                                         "' for material '" + m_name + "'");
            allSucceeded = false;
        }
    }

    // Recompile pipeline state
    m_blendState.Reset();
    m_depthStencilState.Reset();
    m_rasterizerState.Reset();
    m_constantBuffer.Reset();
    m_compiled = false;

    HRESULT hr = CompileMaterial(device);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: failed to recompile material '" + m_name + "'");
        allSucceeded = false;
    }

    if (allSucceeded)
    {
        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' reloaded successfully");
    }

    return allSucceeded;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>

// ============================================================================
// Material (Linux) — Serialization & Reload
// ============================================================================

bool Material::SaveToFile(const std::string& filePath) const
{
    std::ofstream file(filePath);
    if (!file.is_open())
    {
        return false;
    }

    if (m_name.empty())
    {
        return false;
    }

    // Use same INI format as Windows for cross-platform compatibility
    file << "# Spark Engine Material File\n";
    file << "# Version: 1.0\n\n";

    file << "[Material]\n";
    file << "Name=" << m_name << "\n";
    file << "ActiveVariant=" << m_activeVariant << "\n\n";

    file << "[PBR]\n";
    file << "AlbedoColor=" << m_pbrProperties.albedoColor.x << "," << m_pbrProperties.albedoColor.y << ","
         << m_pbrProperties.albedoColor.z << "," << m_pbrProperties.albedoColor.w << "\n";
    file << "MetallicFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.metallicFactor << "\n";
    file << "RoughnessFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.roughnessFactor << "\n";
    file << "NormalScale=" << std::fixed << std::setprecision(6) << m_pbrProperties.normalScale << "\n";
    file << "OcclusionStrength=" << std::fixed << std::setprecision(6) << m_pbrProperties.occlusionStrength << "\n";
    file << "EmissiveColor=" << m_pbrProperties.emissiveColor.x << "," << m_pbrProperties.emissiveColor.y << ","
         << m_pbrProperties.emissiveColor.z << "\n";
    file << "EmissiveFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.emissiveFactor << "\n";
    file << "AlphaCutoff=" << std::fixed << std::setprecision(6) << m_pbrProperties.alphaCutoff << "\n";
    file << "IndexOfRefraction=" << std::fixed << std::setprecision(6) << m_pbrProperties.indexOfRefraction << "\n\n";

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
        file << "SheenColor=" << m_advancedProperties.sheenColor.x << "," << m_advancedProperties.sheenColor.y << ","
             << m_advancedProperties.sheenColor.z << "\n";
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
    file << "BlendMode=" << static_cast<int>(m_renderState.blendMode) << "\n";
    file << "CullMode=" << static_cast<int>(m_renderState.cullMode) << "\n";
    file << "DepthTest=" << (m_renderState.depthTest ? "true" : "false") << "\n";
    file << "DepthWrite=" << (m_renderState.depthWrite ? "true" : "false") << "\n";
    file << "CastShadows=" << (m_renderState.castShadows ? "true" : "false") << "\n";
    file << "ReceiveShadows=" << (m_renderState.receiveShadows ? "true" : "false") << "\n";
    file << "RenderQueue=" << m_renderState.renderQueue << "\n";
    file << "DoubleSided=" << (m_renderState.doubleSided ? "true" : "false") << "\n\n";

    file << "[Textures]\n";
    for (const auto& pair : m_textures)
    {
        if (!pair.second.filePath.empty())
        {
            file << "Texture" << static_cast<int>(pair.first) << "=" << pair.second.filePath << "\n";
            file << "Texture" << static_cast<int>(pair.first) << "_Enabled=" << (pair.second.enabled ? "true" : "false")
                 << "\n";
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
    }

    file.close();
    return true;
}

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* /*device*/)
{
    // On Linux, full material file parsing is not supported.
    // Materials should be created programmatically or loaded via platform tools.
    fprintf(stderr,
            "[MaterialSystem] LoadFromFile: Loading from file '%s' is not "
            "supported on Linux. Use CreateMaterial() and set properties manually.\n",
            filePath.c_str());
    return false;
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
    // On Linux, re-load texture metadata from disk
    for (auto& [type, matTexture] : m_textures)
    {
        if (!matTexture.filePath.empty())
        {
            LoadTexture(type, matTexture.filePath, device);
        }
    }

    m_compiled = false;
    CompileMaterial(device);
    return true;
}

#endif // SPARK_PLATFORM_WINDOWS

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
