/**
 * @file PBRMaterialLightingLinux.cpp
 * @brief Linux Material::LoadFromFile and ReloadMaterial stubs
 *
 * On Linux, material file parsing is not fully supported (no WIC texture loading).
 * Materials should be created programmatically. The Windows counterpart is in
 * PBRMaterialLightingWindows.cpp. Shared code (SaveToFile, GetShaderPermutation)
 * stays in PBRMaterialLighting.cpp.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include "Utils/LocalFileCache.h"
#include <cstdio>
#include <filesystem>

// ============================================================================
// Material (Linux) — LoadFromFile & Reload
// ============================================================================

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* /*device*/)
{
    fprintf(stderr,
            "[MaterialSystem] LoadFromFile: Loading from file '%s' is not "
            "supported on Linux. Use CreateMaterial() and set properties manually.\n",
            filePath.c_str());
    return false;
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
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

#endif // !SPARK_PLATFORM_WINDOWS
