/**
 * @file GraphicsConsoleOpsWindows.cpp
 * @brief Windows/D3D11 implementation — split from GraphicsConsoleOps.cpp
 *
 * Settings, feature toggles, and state-callback console methods. Diagnostics
 * and device operations (screenshot, benchmark, system info, GC, GPU timing,
 * VRAM usage, device reset, shader reload) live in
 * GraphicsConsoleOpsWindowsDiagnostics.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS


#include "GraphicsEngine.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <string>
#include <mutex>
#include <algorithm>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================================
// CONSOLE METHODS IMPLEMENTATION
// ============================================================================

RenderStatistics GraphicsEngine::Console_GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_statistics;
}

void GraphicsEngine::Console_SetQuality(const std::string& preset)
{
    QualityPreset qualityPreset = QualityPreset::Medium;

    if (preset == "low")
        qualityPreset = QualityPreset::Low;
    else if (preset == "medium")
        qualityPreset = QualityPreset::Medium;
    else if (preset == "high")
        qualityPreset = QualityPreset::High;
    else if (preset == "ultra")
        qualityPreset = QualityPreset::Ultra;
    else if (preset == "custom")
        qualityPreset = QualityPreset::Custom;

    SetQualityPreset(qualityPreset);
    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset set to " + std::wstring(preset.begin(), preset.end()), L"INFO");
}

void GraphicsEngine::Console_SetRenderPath(const std::string& path)
{
    RenderPath renderPath = RenderPath::Forward;

    if (path == "forward")
        renderPath = RenderPath::Forward;
    else if (path == "deferred")
        renderPath = RenderPath::Deferred;
    else if (path == "forward_plus")
        renderPath = RenderPath::ForwardPlus;
    else if (path == "clustered")
        renderPath = RenderPath::Clustered;

    SetRenderPath(renderPath);
    LOG_TO_CONSOLE_IMMEDIATE(L"Render path set to " + std::wstring(path.begin(), path.end()), L"INFO");
}

void GraphicsEngine::Console_EnableFeature(const std::string& feature, bool enabled)
{
    if (feature == "vsync")
    {
        m_settings.vsync = enabled;
    }
    else if (feature == "wireframe")
    {
        m_settings.wireframeMode = enabled;
        ApplyGraphicsState();
    }
    else if (feature == "shadows")
    {
        m_settings.shadows = enabled;
        if (m_lightingSystem)
        {
            m_lightingSystem->EnableShadows(enabled);
        }
    }
    else if (feature == "bloom")
    {
        m_settings.bloom = enabled;
    }
    else if (feature == "ssao")
    {
        m_settings.ssao = enabled;
    }
    else if (feature == "taa")
    {
        m_settings.taa = enabled;
    }
    else if (feature == "hdr")
    {
        m_settings.hdr = enabled;
        m_hdrEnabled = enabled;
    }
    else if (feature == "frustum_culling")
    {
        m_settings.frustumCulling = enabled;
    }

    std::wstring featureName(feature.begin(), feature.end());
    std::wstring statusMsg = enabled ? L" enabled" : L" disabled";
    LOG_TO_CONSOLE_IMMEDIATE(featureName + statusMsg, L"INFO");
}

void GraphicsEngine::Console_SetSetting(const std::string& setting, float value)
{
    if (setting == "shadow_map_size")
    {
        m_settings.shadowMapSize = static_cast<uint32_t>(value);
        if (m_lightingSystem)
        {
            m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
        }
    }
    else if (setting == "max_texture_size")
    {
        m_settings.maxTextureSize = static_cast<uint32_t>(value);
    }
    else if (setting == "anisotropy_level")
    {
        m_settings.anisotropyLevel = static_cast<uint32_t>(value);
    }
    else if (setting == "msaa_samples")
    {
        m_settings.msaaSamples = static_cast<uint32_t>(value);
    }
    else if (setting == "render_scale")
    {
        m_settings.renderScale = value;
    }

    std::wstring settingName(setting.begin(), setting.end());
    LOG_TO_CONSOLE_IMMEDIATE(settingName + L" set to " + std::to_wstring(value), L"INFO");
}

void GraphicsEngine::Console_SetWireframe(bool enabled)
{
    m_settings.wireframeMode = enabled;
    ApplyGraphicsState();
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Wireframe mode enabled" : L"Wireframe mode disabled", L"INFO");
}

void GraphicsEngine::Console_SetVSync(bool enabled)
{
    m_settings.vsync = enabled;
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"VSync enabled" : L"VSync disabled", L"INFO");
}

void GraphicsEngine::Console_SetRenderingPipeline(RenderingPipeline pipeline)
{
    SetRenderingPipeline(pipeline);
}

void GraphicsEngine::Console_SetHDR(bool enabled)
{
    SetHDREnabled(enabled);
}

void GraphicsEngine::Console_ApplySettings(const GraphicsSettings& settings)
{
    m_settings = settings;
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();
    NotifyStateChange();
}

void GraphicsEngine::Console_ResetToDefaults()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Resetting graphics settings to defaults via console", L"INFO");

    // Create a new default settings structure
    GraphicsSettings defaults;

    // Apply the default settings
    m_settings = defaults;

    // Apply the graphics state changes
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();

    // Notify state change if callback is registered
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings reset to defaults successfully", L"SUCCESS");
}

void GraphicsEngine::Console_RegisterStateCallback(std::function<void()> callback)
{
    m_stateCallback = callback;
}

void GraphicsEngine::Console_SetDebugMode(bool enabled)
{
    m_settings.debugMode = enabled;
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Debug mode enabled" : L"Debug mode disabled", L"INFO");
}

void GraphicsEngine::Console_SetClearColor(float r, float g, float b, float a)
{
    m_settings.clearColor[0] = (std::max)(0.0f, (std::min)(1.0f, r));
    m_settings.clearColor[1] = (std::max)(0.0f, (std::min)(1.0f, g));
    m_settings.clearColor[2] = (std::max)(0.0f, (std::min)(1.0f, b));
    m_settings.clearColor[3] = (std::max)(0.0f, (std::min)(1.0f, a));
    LOG_TO_CONSOLE_IMMEDIATE(L"Clear color set", L"INFO");
}

void GraphicsEngine::Console_SetRenderScale(float scale)
{
    m_settings.renderScale = (std::max)(0.1f, (std::min)(4.0f, scale));
    LOG_TO_CONSOLE_IMMEDIATE(L"Render scale set to " + std::to_wstring(scale), L"INFO");
}


#endif // SPARK_PLATFORM_WINDOWS
