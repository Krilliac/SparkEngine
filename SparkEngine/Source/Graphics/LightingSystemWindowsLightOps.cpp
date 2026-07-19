/**
 * @file LightingSystemWindowsLightOps.cpp
 * @brief Windows light management and console operations for LightingSystem
 *
 * Light creation/removal, environment map handling, and the Console_*
 * command surface split out of LightingSystemWindows.cpp, which keeps the
 * LightingSystem class lifecycle, per-frame update, data binding, and
 * shadow map rendering. The Linux counterpart lives in
 * LightingSystemLinux.cpp.
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <memory>
#include <string>

using namespace DirectX;

// ============================================================================
// LIGHT MANAGEMENT METHODS
// ============================================================================

std::shared_ptr<Light> LightingSystem::CreateLight(LightType type)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Creating light of type %d", static_cast<int>(type));
    auto light = std::make_shared<Light>(type);
    m_lights.push_back(light);

    // Create shadow map for this light if shadows are enabled
    if (light->GetCastShadows() && m_shadowsEnabled)
    {
        auto shadowMap = std::make_unique<ShadowMap>();
        if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap)))
        {
            m_shadowMaps[light.get()] = std::move(shadowMap);
        }
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Created new light of type: " + std::to_string(static_cast<int>(type)));
    return light;
}

void LightingSystem::AddLight(std::shared_ptr<Light> light)
{
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, light);
    if (light)
    {
        m_lights.push_back(light);

        // Create shadow map if needed
        if (light->GetCastShadows() && m_shadowsEnabled)
        {
            auto shadowMap = std::make_unique<ShadowMap>();
            if (SUCCEEDED(CreateShadowMap(m_shadowMapSize, *shadowMap)))
            {
                m_shadowMaps[light.get()] = std::move(shadowMap);
            }
        }
    }
}

void LightingSystem::RemoveLight(std::shared_ptr<Light> light)
{
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, light);
    if (light)
    {
        // Remove shadow map
        auto it = m_shadowMaps.find(light.get());
        if (it != m_shadowMaps.end())
        {
            m_shadowMaps.erase(it);
        }

        // Remove from lights vector
        m_lights.erase(std::remove(m_lights.begin(), m_lights.end(), light), m_lights.end());
    }
}

void LightingSystem::RemoveAllLights()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Removing all lights (%zu total)", m_lights.size());
    m_shadowMaps.clear();
    m_lights.clear();

    // Recreate default directional light
    auto defaultLight = std::make_shared<Light>(LightType::Directional);
    defaultLight->SetDirection({0.3f, -0.7f, 0.2f});
    defaultLight->SetColor({1.0f, 0.95f, 0.8f});
    defaultLight->SetIntensity(3.0f);
    m_lights.push_back(defaultLight);
}

void LightingSystem::SetEnvironmentMap(const std::string& filePath)
{
    // This would normally load an HDR environment map
    // For now, just log the request
    Spark::SimpleConsole::GetInstance().LogInfo("Environment map set to: " + filePath);

    // Generate IBL textures after loading
    GenerateIBLTextures();
}

void LightingSystem::GenerateIBLTextures()
{
    if (!m_device)
        return;

    // This would normally generate irradiance map, prefilter map, and BRDF LUT
    // For now, just log the operation
    Spark::SimpleConsole::GetInstance().LogInfo("Generating IBL textures");

    HRESULT hr = GenerateIrradianceMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate irradiance map");
        return;
    }
    hr = GeneratePrefilterMap(m_environmentLighting.environmentMap.Get());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate prefilter map");
        return;
    }
    hr = GenerateBRDFLUT();
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to generate BRDF LUT");
        return;
    }
    Spark::SimpleConsole::GetInstance().LogSuccess("IBL textures generated successfully");
}

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

std::string LightingSystem::Console_GetLightInfo(int lightIndex) const
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        return "Error: Invalid light index " + std::to_string(lightIndex);
    }

    const auto& light = m_lights[lightIndex];
    if (!light)
    {
        return "Error: Light at index " + std::to_string(lightIndex) + " is null";
    }

    return "Light [" + std::to_string(lightIndex) + "]:\n" + light->GetInfo();
}

int LightingSystem::Console_CreateLight(const std::string& type)
{
    LightType lightType = StringToLightType(type);
    auto light = CreateLight(lightType);

    if (m_lights.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create light of type: " + type);
        return -1;
    }

    int index = static_cast<int>(m_lights.size() - 1);
    Spark::SimpleConsole::GetInstance().LogSuccess("Created light at index " + std::to_string(index));
    return index;
}

bool LightingSystem::Console_DeleteLight(int lightIndex)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return false;
    }

    auto light = m_lights[lightIndex];
    RemoveLight(light);

    Spark::SimpleConsole::GetInstance().LogSuccess("Deleted light at index " + std::to_string(lightIndex));
    return true;
}

void LightingSystem::Console_SetLightProperty(int lightIndex, const std::string& property, float value)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }

    auto& light = m_lights[lightIndex];
    if (light)
    {
        light->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) +
                                                       " for light " + std::to_string(lightIndex));
    }
}

void LightingSystem::Console_SetLightColor(int lightIndex, float r, float g, float b)
{
    if (lightIndex < 0 || lightIndex >= static_cast<int>(m_lights.size()))
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid light index: " + std::to_string(lightIndex));
        return;
    }

    auto& light = m_lights[lightIndex];
    if (light)
    {
        light->Console_SetColor(r, g, b);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set color for light " + std::to_string(lightIndex));
    }
}

void LightingSystem::Console_SetShadowQuality(const std::string& quality)
{
    uint32_t size = 1024;

    if (quality == "low")
        size = 512;
    else if (quality == "medium")
        size = 1024;
    else if (quality == "high")
        size = 2048;
    else if (quality == "ultra")
        size = 4096;
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Invalid shadow quality: " + quality);
        return;
    }

    SetGlobalShadowQuality(size);
    Spark::SimpleConsole::GetInstance().LogSuccess("Shadow quality set to " + quality);
}

void LightingSystem::Console_SetEnvironment(const std::string& skyType)
{
    if (skyType == "clear")
    {
        m_environmentLighting.skyColor = {0.5f, 0.7f, 1.0f};
        m_environmentLighting.skyIntensity = 1.0f;
        m_environmentLighting.fogEnabled = false;
    }
    else if (skyType == "overcast")
    {
        m_environmentLighting.skyColor = {0.6f, 0.6f, 0.6f};
        m_environmentLighting.skyIntensity = 0.8f;
        m_environmentLighting.fogEnabled = true;
        m_environmentLighting.fogDensity = 0.02f;
    }
    else if (skyType == "sunset")
    {
        m_environmentLighting.skyColor = {1.0f, 0.6f, 0.3f};
        m_environmentLighting.skyIntensity = 1.2f;
        m_environmentLighting.fogEnabled = false;
    }
    else if (skyType == "night")
    {
        m_environmentLighting.skyColor = {0.1f, 0.1f, 0.3f};
        m_environmentLighting.skyIntensity = 0.3f;
        m_environmentLighting.fogEnabled = false;
    }

    Spark::SimpleConsole::GetInstance().LogSuccess("Environment set to " + skyType);
}

void LightingSystem::Console_EnableLightCulling(bool enabled)
{
    EnableLightCulling(enabled);
    Spark::SimpleConsole::GetInstance().LogInfo("Light culling " + std::string(enabled ? "enabled" : "disabled"));
}

void LightingSystem::Console_ReloadIBL()
{
    GenerateIBLTextures();
    Spark::SimpleConsole::GetInstance().LogSuccess("IBL textures reloaded");
}

#endif // SPARK_PLATFORM_WINDOWS
