/**
 * @file GraphicsStateAndSettingsWindowsSettings.cpp
 * @brief D3D11 graphics settings and configuration
 *
 * Contains render path, quality preset, rendering pipeline, HDR, MSAA/TAA/SSAO/SSR,
 * and volumetric settings methods split from GraphicsStateAndSettingsWindows.cpp.
 * Linux counterpart lives in GraphicsStateAndSettingsLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <cstring>
#include <mutex>
#include <string>

// ============================================================================
// SETTINGS AND CONFIGURATION
// ============================================================================

void GraphicsEngine::SetRenderPath(RenderPath path)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting render path", L"INFO");
    m_settings.renderPath = path;

    switch (path)
    {
    case RenderPath::Forward:
        m_currentPipeline = RenderingPipeline::Forward;
        break;
    case RenderPath::Deferred:
        m_currentPipeline = RenderingPipeline::Deferred;
        SetupDeferredPipeline();
        break;
    case RenderPath::ForwardPlus:
        m_currentPipeline = RenderingPipeline::ForwardPlus;
        SetupForwardPlusPipeline();
        break;
    case RenderPath::Clustered:
        m_currentPipeline = RenderingPipeline::Clustered;
        break;
    }

    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Render path changed", L"INFO");
}

void GraphicsEngine::SetQualityPreset(QualityPreset preset)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying quality preset", L"INFO");

    switch (preset)
    {
    case QualityPreset::Low:
        m_settings.msaaSamples = 1;
        m_settings.shadowMapSize = 512;
        m_settings.maxTextureSize = 512;
        m_settings.anisotropyLevel = 1;
        m_settings.shadows = false;
        m_settings.bloom = false;
        m_settings.ssao = false;
        m_settings.taa = false;
        break;

    case QualityPreset::Medium:
        m_settings.msaaSamples = 2;
        m_settings.shadowMapSize = 1024;
        m_settings.maxTextureSize = 1024;
        m_settings.anisotropyLevel = 4;
        m_settings.shadows = true;
        m_settings.bloom = true;
        m_settings.ssao = false;
        m_settings.taa = false;
        break;

    case QualityPreset::High:
        m_settings.msaaSamples = 4;
        m_settings.shadowMapSize = 2048;
        m_settings.maxTextureSize = 2048;
        m_settings.anisotropyLevel = 8;
        m_settings.shadows = true;
        m_settings.bloom = true;
        m_settings.ssao = true;
        m_settings.taa = false;
        break;

    case QualityPreset::Ultra:
        m_settings.msaaSamples = 8;
        m_settings.shadowMapSize = 4096;
        m_settings.maxTextureSize = 4096;
        m_settings.anisotropyLevel = 16;
        m_settings.shadows = true;
        m_settings.bloom = true;
        m_settings.ssao = true;
        m_settings.taa = true;
        break;

    case QualityPreset::Custom:
        LOG_TO_CONSOLE_IMMEDIATE(L"Custom quality preset selected - no changes applied", L"INFO");
        return;
    }

    m_settings.qualityPreset = preset;
    ApplyQualityPreset(preset);
    ApplyGraphicsState();
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset applied successfully", L"SUCCESS");
}

void GraphicsEngine::SetRenderingPipeline(RenderingPipeline pipeline)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting rendering pipeline", L"INFO");
    m_currentPipeline = pipeline;

    switch (pipeline)
    {
    case RenderingPipeline::Forward:
        m_settings.renderPath = RenderPath::Forward;
        break;
    case RenderingPipeline::Deferred:
        m_settings.renderPath = RenderPath::Deferred;
        SetupDeferredPipeline();
        break;
    case RenderingPipeline::ForwardPlus:
        m_settings.renderPath = RenderPath::ForwardPlus;
        SetupForwardPlusPipeline();
        break;
    case RenderingPipeline::Clustered:
        m_settings.renderPath = RenderPath::Clustered;
        break;
    }

    NotifyStateChange();
}

void GraphicsEngine::SetHDREnabled(bool enabled)
{
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Enabling HDR" : L"Disabling HDR", L"INFO");
    m_hdrEnabled = enabled;
    m_settings.hdr = enabled;
    NotifyStateChange();
}

void GraphicsEngine::SetMSAALevel(MSAALevel msaaLevel)
{
    m_msaaLevel = msaaLevel;
    m_settings.msaaSamples = static_cast<uint32_t>(msaaLevel);
}

void GraphicsEngine::SetTAASettings(const TAASettings& settings)
{
    m_taaSettings = settings;
    m_settings.taa = settings.enabled;
}

void GraphicsEngine::SetSSAOSettings(const SSAOSettings& settings)
{
    m_ssaoSettings = settings;
    m_settings.ssao = settings.enabled;
}

void GraphicsEngine::SetSSRSettings(const SSRSettings& settings)
{
    m_ssrSettings = settings;
}

void GraphicsEngine::SetVolumetricSettings(const VolumetricSettings& settings)
{
    m_volumetricSettings = settings;
}

void GraphicsEngine::SetGraphicsSettings(const GraphicsSettings& settings)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_settings = settings;
}

void GraphicsEngine::NotifyStateChange()
{
    if (m_stateCallback)
    {
        try
        {
            m_stateCallback();
        }
        catch (const std::exception& e)
        {
            LOG_TO_CONSOLE_IMMEDIATE(
                L"Error in state change callback: " + std::wstring(e.what(), e.what() + strlen(e.what())), L"ERROR");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Unknown error in state change callback", L"ERROR");
        }
    }
}

void GraphicsEngine::ApplyQualityPreset(QualityPreset preset)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying quality preset configurations", L"INFO");

    // Apply preset-specific configurations to subsystems
    if (m_lightingSystem)
    {
        m_lightingSystem->EnableShadows(m_settings.shadows);
        m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
    }

    if (m_materialSystem)
    {
        std::string quality;
        switch (preset)
        {
        case QualityPreset::Low:
            quality = "low";
            break;
        case QualityPreset::Medium:
            quality = "medium";
            break;
        case QualityPreset::High:
            quality = "high";
            break;
        case QualityPreset::Ultra:
            quality = "ultra";
            break;
        default:
            quality = "medium";
            break;
        }
        m_materialSystem->Console_SetTextureQuality(quality);
    }

    if (m_textureSystem)
    {
        std::string quality;
        switch (preset)
        {
        case QualityPreset::Low:
            quality = "low";
            break;
        case QualityPreset::Medium:
            quality = "medium";
            break;
        case QualityPreset::High:
            quality = "high";
            break;
        case QualityPreset::Ultra:
            quality = "ultra";
            break;
        default:
            quality = "medium";
            break;
        }
        m_textureSystem->Console_SetQuality(quality);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset configurations applied", L"INFO");
}

#endif // SPARK_PLATFORM_WINDOWS
