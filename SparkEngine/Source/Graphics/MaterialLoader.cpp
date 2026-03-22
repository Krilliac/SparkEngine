/**
 * @file MaterialLoader.cpp
 * @brief Data-driven material loading from .sparkmat text files
 */

#include "MaterialLoader.h"
#include "MaterialSystem.h"
#include "../Utils/SparkConsole.h"

#include "../Utils/StringUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace Spark::Graphics
{

    // =========================================================================
    // Singleton
    // =========================================================================

    MaterialLoader& MaterialLoader::GetInstance()
    {
        static MaterialLoader s;
        return s;
    }

    void MaterialLoader::Shutdown()
    {
        m_loadedNames.clear();
    }

    // =========================================================================
    // Public API
    // =========================================================================

    bool MaterialLoader::LoadMaterial(const std::string& filePath)
    {
        MaterialDefinition def;
        if (!ParseFile(filePath, def))
        {
            return false;
        }

        if (def.name.empty())
        {
            // Derive name from filename if not specified
            def.name = fs::path(filePath).stem().string();
        }

        if (!RegisterMaterial(def))
        {
            return false;
        }

        m_loadedNames.push_back(def.name);
        return true;
    }

    bool MaterialLoader::LoadMaterialsFromDirectory(const std::string& dirPath)
    {
        std::error_code ec;
        if (!fs::is_directory(dirPath, ec))
        {
            Spark::SimpleConsole::GetInstance().LogWarning("MaterialLoader: not a directory: " + dirPath);
            return false;
        }

        bool anyLoaded = false;
        for (const auto& entry : fs::directory_iterator(dirPath, ec))
        {
            if (!entry.is_regular_file())
                continue;

            if (entry.path().extension() == ".sparkmat")
            {
                if (LoadMaterial(entry.path().string()))
                {
                    anyLoaded = true;
                }
            }
        }

        return anyLoaded;
    }

    // =========================================================================
    // File Parsing
    // =========================================================================

    /// Trim leading and trailing whitespace from a string.
    static std::string TrimWhitespace(const std::string& str)
    {
        auto start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        auto end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    bool MaterialLoader::ParseFile(const std::string& filePath, MaterialDefinition& outDef) const
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("MaterialLoader: failed to open file: " + filePath);
            return false;
        }

        outDef = {};
        std::string line;
        while (std::getline(file, line))
        {
            // Skip empty lines and comments
            std::string trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//"))
                continue;

            // Split on first '='
            auto eqPos = trimmed.find('=');
            if (eqPos == std::string::npos)
                continue;

            std::string key = TrimWhitespace(trimmed.substr(0, eqPos));
            std::string value = TrimWhitespace(trimmed.substr(eqPos + 1));

            if (key == "name")
                outDef.name = value;
            else if (key == "blendMode")
                outDef.blendMode = value;
            else if (key == "metallic")
                outDef.metallic = Spark::StringUtils::ParseFloat(value).value_or(outDef.metallic);
            else if (key == "roughness")
                outDef.roughness = Spark::StringUtils::ParseFloat(value).value_or(outDef.roughness);
            else if (key == "normalScale")
                outDef.normalScale = Spark::StringUtils::ParseFloat(value).value_or(outDef.normalScale);
            else if (key == "occlusionStrength")
                outDef.occlusionStrength = Spark::StringUtils::ParseFloat(value).value_or(outDef.occlusionStrength);
            else if (key == "emissiveFactor")
                outDef.emissiveFactor = Spark::StringUtils::ParseFloat(value).value_or(outDef.emissiveFactor);
            else if (key == "alphaCutoff")
                outDef.alphaCutoff = Spark::StringUtils::ParseFloat(value).value_or(outDef.alphaCutoff);
            else if (key == "albedoTexture")
                outDef.albedoTexture = value;
            else if (key == "normalTexture")
                outDef.normalTexture = value;
            else if (key == "metallicTexture")
                outDef.metallicTexture = value;
            else if (key == "roughnessTexture")
                outDef.roughnessTexture = value;
            else if (key == "emissiveTexture")
                outDef.emissiveTexture = value;
            else if (key == "occlusionTexture")
                outDef.occlusionTexture = value;
        }

        return !outDef.name.empty();
    }

    // =========================================================================
    // Material Registration
    // =========================================================================

    static BlendMode ParseBlendMode(const std::string& str)
    {
        if (str == "AlphaTest")
            return BlendMode::AlphaTest;
        if (str == "Transparent")
            return BlendMode::Transparent;
        if (str == "Additive")
            return BlendMode::Additive;
        if (str == "Multiply")
            return BlendMode::Multiply;
        if (str == "Screen")
            return BlendMode::Screen;
        return BlendMode::Opaque;
    }

    bool MaterialLoader::RegisterMaterial(const MaterialDefinition& def)
    {
        // Access the global MaterialSystem to create the material.
        // MaterialSystem is expected to be initialized before loading materials.
        auto mat = std::make_shared<Material>(def.name);

        PBRProperties pbr;
        pbr.metallicFactor = std::clamp(def.metallic, 0.0f, 1.0f);
        pbr.roughnessFactor = std::clamp(def.roughness, 0.0f, 1.0f);
        pbr.normalScale = def.normalScale;
        pbr.occlusionStrength = std::clamp(def.occlusionStrength, 0.0f, 1.0f);
        pbr.emissiveFactor = std::max(0.0f, def.emissiveFactor);
        pbr.alphaCutoff = std::clamp(def.alphaCutoff, 0.0f, 1.0f);
        mat->SetPBRProperties(pbr);

        MaterialRenderState renderState;
        renderState.blendMode = ParseBlendMode(def.blendMode);
        mat->SetRenderState(renderState);

        // Store texture file paths for deferred loading by the MaterialSystem.
        // Actual GPU texture creation requires a D3D11 device, which the
        // MaterialSystem owns. We set the file path so LoadTexture() can
        // load them when a device is available.
        if (!def.albedoTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.albedoTexture;
            mat->SetTexture(MaterialTextureType::Albedo, tex);
        }
        if (!def.normalTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.normalTexture;
            mat->SetTexture(MaterialTextureType::Normal, tex);
        }
        if (!def.metallicTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.metallicTexture;
            mat->SetTexture(MaterialTextureType::Metallic, tex);
        }
        if (!def.roughnessTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.roughnessTexture;
            mat->SetTexture(MaterialTextureType::Roughness, tex);
        }
        if (!def.emissiveTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.emissiveTexture;
            mat->SetTexture(MaterialTextureType::Emissive, tex);
        }
        if (!def.occlusionTexture.empty())
        {
            MaterialTexture tex;
            tex.filePath = def.occlusionTexture;
            mat->SetTexture(MaterialTextureType::Occlusion, tex);
        }

        Spark::SimpleConsole::GetInstance().LogSuccess("MaterialLoader: loaded material '" + def.name + "'");
        return true;
    }

} // namespace Spark::Graphics
