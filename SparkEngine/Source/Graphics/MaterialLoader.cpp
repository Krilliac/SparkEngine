/**
 * @file MaterialLoader.cpp
 * @brief Data-driven material loading from .sparkmat text files
 */

#include "MaterialLoader.h"
#include "MaterialSystem.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#include "../Utils/FileUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
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
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loading material from file: %s", filePath.c_str());
        SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadBegin, filePath, 0.0);
        SparkMatDefinition def;
        if (!ParseFile(filePath, def))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to parse material file: %s", filePath.c_str());
            return false;
        }

        if (def.name.empty())
        {
            // Derive name from filename if not specified
            def.name = fs::path(filePath).stem().string();
        }

        RegisterMaterial(def);

        m_loadedNames.push_back(def.name);
        SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadComplete, filePath, 0.0);
        return true;
    }

    bool MaterialLoader::LoadMaterialsFromDirectory(const std::string& dirPath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Scanning directory for materials: %s", dirPath.c_str());
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
                // LoadMaterial() reopens the file through a narrow path. path::string()
                // throws on Windows for a name the ANSI code page cannot spell, which
                // used to abort the scan (and the engine start that runs it).
                const std::optional<std::string> narrow = Spark::FileUtils::TryPathToNarrow(entry.path());
                if (!narrow)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "MaterialLoader: skipping '%s': its name has no spelling in the active code page",
                                   Spark::FileUtils::TryPathToUtf8(entry.path()).value_or("?").c_str());
                    continue;
                }
                if (LoadMaterial(*narrow))
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

    bool MaterialLoader::ParseFile(const std::string& filePath, SparkMatDefinition& outDef) const
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "MaterialLoader: failed to open file: %s", filePath.c_str());
            Spark::SimpleConsole::GetInstance().LogWarning("MaterialLoader: failed to open file: " + filePath);
            return false;
        }

        return ParseSparkMatDefinition(file, outDef);
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

    void MaterialLoader::RegisterMaterial(const SparkMatDefinition& def)
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

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Registered material '%s' (metallic=%.2f, roughness=%.2f)",
                       def.name.c_str(), def.metallic, def.roughness);
        Spark::SimpleConsole::GetInstance().LogSuccess("MaterialLoader: loaded material '" + def.name + "'");
    }

} // namespace Spark::Graphics
