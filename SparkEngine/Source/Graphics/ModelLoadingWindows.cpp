/**
 * @file ModelLoadingWindows.cpp
 * @brief Windows/D3D11 model loading — split from ModelLoading.cpp
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file ModelLoading.cpp
 * @brief Model file parsing, GPU buffer upload, and rendering helpers
 *
 * Contains format-specific loaders (OBJ, FBX, glTF), file-to-asset creation
 * helpers, and D3D11 mesh/material binding for draw calls.
 * Split from AssetPipeline.cpp for maintainability.
 */

#include "AssetPipeline.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// FILE-TO-ASSET CREATION HELPERS (Windows)
// ============================================================================

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loading mesh from file: %s", path.c_str());
    auto meshAsset = std::make_shared<MeshAsset>(path);
    HRESULT hr = meshAsset->Load(m_device);

    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load mesh: %s", path.c_str());
        Spark::SimpleConsole::GetInstance().LogError("Failed to load mesh: " + path);
        return nullptr;
    }

    return meshAsset;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto textureAsset = std::make_shared<TextureAsset>(path);
    HRESULT hr = textureAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + path);
        return nullptr;
    }

    return textureAsset;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audioAsset = std::make_shared<AudioAsset>(path);
    HRESULT hr = audioAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load audio: " + path);
        return nullptr;
    }

    return audioAsset;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#endif // SPARK_PLATFORM_WINDOWS
