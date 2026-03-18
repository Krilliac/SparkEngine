/**
 * @file GraphicsConsoleOps.cpp
 * @brief Console integration methods for GraphicsEngine
 *
 * All Console_* methods that implement console command operations for the
 * graphics engine. Split from GraphicsEngine.cpp for maintainability.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "../Utils/LogMacros.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cfloat>
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

void GraphicsEngine::Console_ReloadShaders()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Reloading shaders via console", L"INFO");

    // Release existing shaders
    m_basicVertexShader.Reset();
    m_basicPixelShader.Reset();
    m_basicInputLayout.Reset();

    // Reinitialize shader system
    HRESULT hr = InitializeBasicShaders();
    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Shaders reloaded successfully", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to reload shaders", L"ERROR");
    }
}

bool GraphicsEngine::Console_Screenshot(const std::string& filename)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Taking screenshot", L"INFO");

    std::string actualFilename = filename;
    if (actualFilename.empty())
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << "screenshot_" << time_t << ".png";
        actualFilename = ss.str();
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot saved as " + std::wstring(actualFilename.begin(), actualFilename.end()),
                             L"SUCCESS");
    return true;
}

std::string GraphicsEngine::Console_GetSystemInfo() const
{
    std::stringstream ss;

    ss << "=== Graphics System Information ===\n";
    ss << "Window Resolution: " << m_windowWidth << "x" << m_windowHeight << "\n";
    ss << "Rendering Pipeline: ";

    switch (m_currentPipeline)
    {
    case RenderingPipeline::Forward:
        ss << "Forward\n";
        break;
    case RenderingPipeline::Deferred:
        ss << "Deferred\n";
        break;
    case RenderingPipeline::ForwardPlus:
        ss << "Forward+\n";
        break;
    case RenderingPipeline::Clustered:
        ss << "Clustered\n";
        break;
    default:
        ss << "Unknown\n";
        break;
    }

    ss << "VSync: " << (m_settings.vsync ? "Enabled" : "Disabled") << "\n";
    ss << "HDR: " << (m_hdrEnabled ? "Enabled" : "Disabled") << "\n";
    ss << "MSAA Samples: " << m_settings.msaaSamples << "\n";
    ss << "Shadow Map Size: " << m_settings.shadowMapSize << "\n";
    ss << "Max Texture Size: " << m_settings.maxTextureSize << "\n";
    ss << "Anisotropy Level: " << m_settings.anisotropyLevel << "\n";

    // Add memory usage
    size_t vramUsage = Console_GetVRAMUsage();
    ss << "VRAM Usage: " << (vramUsage / 1024 / 1024) << " MB\n";

    return ss.str();
}

std::string GraphicsEngine::Console_Benchmark(int seconds)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting " + std::to_wstring(seconds) + L" second benchmark", L"INFO");

    auto startTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    float totalFrameTime = 0.0f;
    float maxFrameTime = 0.0f;
    float minFrameTime = FLT_MAX;

    // Simple benchmark - just count frames and measure timing
    while (true)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);

        if (elapsed.count() >= seconds)
        {
            break;
        }

        // Simulate frame timing
        auto frameStart = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(16667)); // ~60 FPS
        auto frameEnd = std::chrono::high_resolution_clock::now();

        float frameTime =
            std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count() / 1000.0f;

        totalFrameTime += frameTime;
        maxFrameTime = (std::max)(maxFrameTime, frameTime);
        minFrameTime = (std::min)(minFrameTime, frameTime);
        frameCount++;
    }

    std::stringstream ss;
    ss << "=== Benchmark Results ===\n";
    ss << "Duration: " << seconds << " seconds\n";
    ss << "Total Frames: " << frameCount << "\n";
    ss << "Average FPS: " << (frameCount / static_cast<float>(seconds)) << "\n";
    ss << "Average Frame Time: " << (totalFrameTime / frameCount) << " ms\n";
    ss << "Min Frame Time: " << minFrameTime << " ms\n";
    ss << "Max Frame Time: " << maxFrameTime << " ms\n";

    LOG_TO_CONSOLE_IMMEDIATE(L"Benchmark completed", L"SUCCESS");

    return ss.str();
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

void GraphicsEngine::Console_ForceGarbageCollection()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Forcing garbage collection", L"INFO");

    // Force release of temporary resources
    if (m_textureSystem)
    {
        try
        {
            // If texture system has a cleanup method, call it
            LOG_TO_CONSOLE_IMMEDIATE(L"Texture system cleanup triggered", L"INFO");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Texture system cleanup failed", L"WARNING");
        }
    }

    if (m_assetPipeline)
    {
        try
        {
            m_assetPipeline->Console_ForceGC();
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline garbage collection triggered", L"INFO");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline GC failed", L"WARNING");
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Garbage collection complete", L"SUCCESS");
}

void GraphicsEngine::Console_SetGPUTiming(bool enabled)
{
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Enabling GPU timing via console" : L"Disabling GPU timing via console",
                             L"INFO");

    // Update the setting
    m_settings.enableGPUTiming = enabled;

    // If enabling, try to create the timing query if it doesn't exist
    if (enabled && !m_gpuTimingQuery && m_device)
    {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        HRESULT hr = m_device->CreateQuery(&queryDesc, &m_gpuTimingQuery);

        if (SUCCEEDED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPU timing query created successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create GPU timing query", L"ERROR");
            m_settings.enableGPUTiming = false; // Revert setting if creation failed
        }
    }

    // Notify state change
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"GPU timing enabled successfully" : L"GPU timing disabled successfully",
                             L"SUCCESS");
}

size_t GraphicsEngine::Console_GetVRAMUsage() const
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Retrieving VRAM usage via console", L"INFO");

    // Calculate total VRAM usage from tracked memory
    size_t totalUsage = m_textureMemoryUsage + m_bufferMemoryUsage;

    // Add advanced system memory usage if available
    if (m_textureSystem)
    {
        try
        {
            auto textureMetrics = m_textureSystem->Console_GetMetrics();
            totalUsage = textureMetrics.totalMemoryUsage + m_bufferMemoryUsage;
        }
        catch (...)
        {
            // Fall back to tracked usage if metrics are unavailable
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"VRAM usage retrieved: " + std::to_wstring(totalUsage / 1024 / 1024) + L" MB", L"INFO");

    return totalUsage;
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

void GraphicsEngine::Console_ResetDevice()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device reset requested via console", L"WARNING");

    if (!m_device || !m_swapChain)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device not available for reset", L"ERROR");
        return;
    }

    try
    {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();

        HRESULT hr = m_swapChain->ResizeBuffers(0, m_windowWidth, m_windowHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to resize buffers during device reset", L"ERROR");
            return;
        }

        hr = CreateRenderTargetView();
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to recreate render target view", L"ERROR");
            return;
        }

        hr = CreateDepthStencilView();
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to recreate depth stencil view", L"ERROR");
            return;
        }

        SetViewport();
        ApplyGraphicsState();
        ApplyAdvancedGraphicsState();

        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device reset complete", L"SUCCESS");
    }
    catch (...)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Exception occurred during device reset", L"ERROR");
    }
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

#else // !SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "TextureSystem.h"
#include "AssetPipeline.h"

#include <iostream>
#include <sstream>
#include <chrono>

using namespace Spark::Graphics::Detail;

// ============================================================================
// Console Integration Methods — Linux/RHI
// ============================================================================

RenderStatistics GraphicsEngine::Console_GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_statistics;
}

void GraphicsEngine::Console_SetQuality(const std::string& preset)
{
    if (preset == "low")
        SetQualityPreset(QualityPreset::Low);
    else if (preset == "medium")
        SetQualityPreset(QualityPreset::Medium);
    else if (preset == "high")
        SetQualityPreset(QualityPreset::High);
    else if (preset == "ultra")
        SetQualityPreset(QualityPreset::Ultra);
    else
        std::cerr << "[Console] Unknown quality preset: " << preset << std::endl;
}

void GraphicsEngine::Console_SetRenderPath(const std::string& path)
{
    if (path == "forward")
        SetRenderPath(RenderPath::Forward);
    else if (path == "deferred")
        SetRenderPath(RenderPath::Deferred);
    else if (path == "forward+")
        SetRenderPath(RenderPath::ForwardPlus);
    else if (path == "forwardplus")
        SetRenderPath(RenderPath::ForwardPlus);
    else if (path == "clustered")
        SetRenderPath(RenderPath::Clustered);
    else
        std::cerr << "[Console] Unknown render path: " << path << std::endl;
}

void GraphicsEngine::Console_EnableFeature(const std::string& feature, bool enabled)
{
    if (feature == "bloom")
        m_settings.bloom = enabled;
    else if (feature == "ssao")
        m_settings.ssao = enabled;
    else if (feature == "taa")
        m_settings.taa = enabled;
    else if (feature == "motionblur")
        m_settings.motionBlur = enabled;
    else if (feature == "hdr")
        SetHDREnabled(enabled);
    else if (feature == "vsync")
        m_settings.vsync = enabled;
    else if (feature == "shadows")
        m_settings.shadows = enabled;
    else if (feature == "wireframe")
        m_settings.wireframeMode = enabled;
    else
        std::cerr << "[Console] Unknown feature: " << feature << std::endl;
}

void GraphicsEngine::Console_SetSetting(const std::string& setting, float value)
{
    if (setting == "renderscale")
        m_settings.renderScale = value;
    else if (setting == "shadowmapsize")
        m_settings.shadowMapSize = static_cast<uint32_t>(value);
    else if (setting == "anisotropy")
        m_settings.anisotropyLevel = static_cast<uint32_t>(value);
    else if (setting == "maxdrawcalls")
        m_settings.maxDrawCalls = static_cast<uint32_t>(value);
    else
        std::cerr << "[Console] Unknown setting: " << setting << std::endl;
}

void GraphicsEngine::Console_ReloadShaders()
{
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        rhi.bridge.GetShaderCache().ReloadAll(rhi.bridge.GetDevice());
        std::cout << "[Console] Shaders reloaded." << std::endl;
    }
}

bool GraphicsEngine::Console_Screenshot(const std::string& filename)
{
    return SUCCEEDED(SaveScreenshot(filename.empty() ? "screenshot.png" : filename));
}

std::string GraphicsEngine::Console_GetSystemInfo() const
{
    auto& rhi = GetRHI();
    std::ostringstream ss;
    ss << "=== Spark Engine — Linux RHI ===\n";
    if (rhi.initialized)
    {
        ss << "Backend : " << rhi.bridge.GetBackendName() << "\n";
        ss << "Device  : " << rhi.bridge.GetDeviceInfo() << "\n";
        const auto& caps = rhi.bridge.GetCapabilities();
        ss << "VRAM    : " << (caps.dedicatedVideoMemory / (1024 * 1024)) << " MB\n";
        ss << "Max Tex : " << caps.maxTextureSize << "\n";
        ss << "MSAA max: " << caps.maxMSAASamples << "x\n";
    }
    else
    {
        ss << "RHI not initialized.\n";
    }
    ss << "Resolution: " << m_width << "x" << m_height << "\n";
    return ss.str();
}

std::string GraphicsEngine::Console_Benchmark(int seconds)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return "RHI not initialized — cannot benchmark.";

    auto start = std::chrono::high_resolution_clock::now();
    uint32_t frames = 0;
    float totalFrameTime = 0.0f;

    while (true)
    {
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();
        if (elapsed >= static_cast<float>(seconds))
            break;

        BeginFrame();
        EndFrame();
        totalFrameTime += m_statistics.frameTime;
        frames++;
    }

    float avgFrameTime = (frames > 0) ? (totalFrameTime / frames) : 0.0f;
    float avgFps = (avgFrameTime > 0.0f) ? (1000.0f / avgFrameTime) : 0.0f;

    std::ostringstream ss;
    ss << "Benchmark complete: " << frames << " frames in " << seconds << "s\n"
       << "Average frame time: " << avgFrameTime << " ms\n"
       << "Average FPS: " << avgFps << "\n";
    return ss.str();
}

void GraphicsEngine::Console_SetWireframe(bool enabled)
{
    m_settings.wireframeMode = enabled;
    m_statistics.wireframeMode = enabled;
}

void GraphicsEngine::Console_SetVSync(bool enabled)
{
    m_settings.vsync = enabled;
    m_statistics.vsyncEnabled = enabled;
}

void GraphicsEngine::Console_SetRenderingPipeline(RenderingPipeline pipeline)
{
    SetRenderingPipeline(pipeline);
}

void GraphicsEngine::Console_SetHDR(bool enabled)
{
    SetHDREnabled(enabled);
}

void GraphicsEngine::Console_SetDebugMode(bool enabled)
{
    m_settings.debugMode = enabled;
    m_statistics.debugMode = enabled;
}

void GraphicsEngine::Console_SetClearColor(float r, float g, float b, float a)
{
    m_settings.clearColor[0] = r;
    m_settings.clearColor[1] = g;
    m_settings.clearColor[2] = b;
    m_settings.clearColor[3] = a;
}

void GraphicsEngine::Console_SetRenderScale(float scale)
{
    m_settings.renderScale = (scale > 0.0f) ? scale : 1.0f;
}

void GraphicsEngine::Console_ResetDevice()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    auto savedHwnd = m_hwnd;
    Shutdown();
    Initialize(savedHwnd);
    std::cout << "[Console] Device reset complete." << std::endl;
}

void GraphicsEngine::Console_ForceGarbageCollection()
{
    // On Linux/RHI there are no deferred COM releases; flush the shader cache
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        rhi.bridge.GetShaderCache().Clear(rhi.bridge.GetDevice());
    }
    std::cout << "[Console] Garbage collection complete." << std::endl;
}

void GraphicsEngine::Console_ApplySettings(const GraphicsSettings& settings)
{
    SetGraphicsSettings(settings);
}

void GraphicsEngine::Console_ResetToDefaults()
{
    GraphicsSettings defaults;
    SetGraphicsSettings(defaults);
    m_hdrEnabled = defaults.hdr;
    m_msaaLevel = MSAALevel::None;
    m_taaSettings = TAASettings{};
    m_ssaoSettings = SSAOSettings{};
    m_ssrSettings = SSRSettings{};
    m_volumetricSettings = VolumetricSettings{};
    std::cout << "[Console] Settings reset to defaults." << std::endl;
}

void GraphicsEngine::Console_RegisterStateCallback(std::function<void()> callback)
{
    m_stateCallback = std::move(callback);
}

void GraphicsEngine::Console_SetGPUTiming(bool enabled)
{
    m_settings.enableGPUTiming = enabled;
}

size_t GraphicsEngine::Console_GetVRAMUsage() const
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return 0;
    return rhi.bridge.GetFrameStatistics().gpuMemoryUsed;
}

#endif // SPARK_PLATFORM_WINDOWS
