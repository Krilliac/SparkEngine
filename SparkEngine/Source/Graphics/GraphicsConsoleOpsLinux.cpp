/**
 * @file GraphicsConsoleOpsLinux.cpp
 * @brief Linux implementation — split from GraphicsConsoleOps.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "TextureSystem.h"
#include "AssetPipeline.h"
#include "../Utils/Validate.h"

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
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Unknown quality preset: %s", preset.c_str());
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
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Unknown render path: %s", path.c_str());
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
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Unknown feature: %s", feature.c_str());
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
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Unknown setting: %s", setting.c_str());
}

void GraphicsEngine::Console_ReloadShaders()
{
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        rhi.bridge.GetShaderCache().ReloadAll(rhi.bridge.GetDevice());
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shaders reloaded");
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
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Device reset complete");
}

void GraphicsEngine::Console_ForceGarbageCollection()
{
    // On Linux/RHI there are no deferred COM releases; flush the shader cache
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        rhi.bridge.GetShaderCache().Clear(rhi.bridge.GetDevice());
    }
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Garbage collection complete");
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
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Settings reset to defaults");
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


#endif // !SPARK_PLATFORM_WINDOWS
