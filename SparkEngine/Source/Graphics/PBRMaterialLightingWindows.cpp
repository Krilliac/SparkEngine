/**
 * @file PBRMaterialLightingWindows.cpp
 * @brief Windows/D3D11 Material::LoadFromFile and ReloadMaterial
 *
 * Full INI-format material file parser with texture loading, variant parsing,
 * and pipeline state recompilation. The Linux counterpart is in
 * PBRMaterialLightingLinux.cpp. Shared code (SaveToFile, GetShaderPermutation)
 * stays in PBRMaterialLighting.cpp.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include "Utils/LocalFileCache.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>

// ============================================================================
// MATERIAL CLASS — LoadFromFile & Reload (Windows)
// ============================================================================

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* device)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loading material from file: %s", filePath.c_str());
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

        auto safeStoi = [](const std::string& value, int defaultVal = 0) -> int
        {
            try
            {
                return std::stoi(value);
            }
            catch (const std::exception&)
            {
                return defaultVal;
            }
        };

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

        auto parseBool = [](const std::string& value) -> bool
        { return value == "true" || value == "1" || value == "yes"; };

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

            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            if (line[0] == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

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
                        m_renderState.blendMode = static_cast<BlendMode>(safeStoi(value));
                    }
                    else if (key == "CullMode")
                    {
                        m_renderState.cullMode = static_cast<CullMode>(safeStoi(value));
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
                        m_renderState.renderQueue = safeStoi(value);
                    }
                    else if (key == "DoubleSided")
                    {
                        m_renderState.doubleSided = parseBool(value);
                    }
                }
                else if (currentSection == "Textures")
                {
                    if (key.substr(0, 7) == "Texture" && !key.contains("_"))
                    {
                        int textureType = safeStoi(key.substr(7));
                        LoadTexture(static_cast<MaterialTextureType>(textureType), value, device);
                    }
                    else if (key.contains("_Enabled"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Enabled"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = safeStoi(baseKey.substr(7));
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
                            int textureType = safeStoi(baseKey.substr(7));
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
                            int textureType = safeStoi(baseKey.substr(7));
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
                            int textureType = safeStoi(baseKey.substr(7));
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

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Material '%s' loaded from '%s' (textures: %zu, variants: %zu)",
                       m_name.c_str(), filePath.c_str(), m_textures.size(), m_variants.size());
        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' loaded from: " + filePath +
                                                       " (textures: " + std::to_string(m_textures.size()) +
                                                       ", variants: " + std::to_string(m_variants.size()) + ")");

        return true;
    }
    catch (const std::exception& e)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Exception loading material from '%s': %s", filePath.c_str(),
                        e.what());
        Spark::SimpleConsole::GetInstance().LogError("Exception while loading material from " + filePath + ": " +
                                                     std::string(e.what()));
        return false;
    }
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Reloading material: %s", m_name.c_str());
    if (!device)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "ReloadMaterial: device is null for '%s'", m_name.c_str());
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: device is null for material '" + m_name + "'");
        return false;
    }

    bool allSucceeded = true;

    std::vector<std::pair<MaterialTextureType, std::string>> texturesToReload;
    for (const auto& [type, matTexture] : m_textures)
    {
        if (!matTexture.filePath.empty())
        {
            texturesToReload.emplace_back(type, matTexture.filePath);
        }
    }

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
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Material '%s' reloaded successfully", m_name.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' reloaded successfully");
    }

    return allSucceeded;
}

#endif // SPARK_PLATFORM_WINDOWS
