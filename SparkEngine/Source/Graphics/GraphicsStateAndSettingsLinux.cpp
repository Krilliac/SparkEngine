/**
 * @file GraphicsStateAndSettingsLinux.cpp
 * @brief Linux RHI-bridge graphics state management, metrics, and settings
 *
 * Windows counterpart lives in GraphicsStateAndSettingsWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "../Utils/Validate.h"

#if defined(SPARK_OPENGL_SUPPORT)
#include <glad/glad.h>
#endif

#if SPARK_HAS_STB_IMAGE
#include <stb_image_write.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

using namespace Spark::Graphics::Detail;

// ============================================================================
// State and Metrics — Linux/RHI
// ============================================================================

void GraphicsEngine::ApplyGraphicsState()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    // Apply viewport
    SetViewport();

    // Bind the back buffer and depth buffer as render targets
    Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
    Spark::RHI::IRHITexture* depthBuffer = rhi.bridge.GetDepthBuffer();
    if (backBuffer)
    {
        cmd->SetRenderTargets(&backBuffer, 1, depthBuffer);
    }
}

void GraphicsEngine::ApplyAdvancedGraphicsState()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    // Apply the base graphics state first
    ApplyGraphicsState();

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    // Set primitive topology based on current settings
    cmd->SetPrimitiveTopology(Spark::RHI::RHIPrimitiveTopology::TriangleList);
}

void GraphicsEngine::UpdateMetrics()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    const auto& rhiStats = rhi.bridge.GetFrameStatistics();
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_statistics.drawCalls = rhiStats.drawCalls;
    m_statistics.triangles = rhiStats.trianglesRendered;
    m_statistics.vertices = rhiStats.verticesProcessed;
    m_statistics.textureBinds = rhiStats.textureBinds;
    m_statistics.gpuTime = rhiStats.gpuFrameTime;
    m_statistics.totalGPUMemory = rhiStats.gpuMemoryUsed;
}

void GraphicsEngine::UpdateAdvancedMetrics()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    const auto& rhiStats = rhi.bridge.GetFrameStatistics();
    const auto& caps = rhi.bridge.GetCapabilities();

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_statistics.textureMemory = m_textureMemoryUsage;
    m_statistics.bufferMemory = m_bufferMemoryUsage;
    m_statistics.totalGPUMemory = rhiStats.gpuMemoryUsed;
    m_statistics.materialSwitches = rhiStats.pipelineChanges;
    m_statistics.vsyncEnabled = m_settings.vsync;
    m_statistics.wireframeMode = m_settings.wireframeMode;
    m_statistics.debugMode = m_settings.debugMode;
}

void GraphicsEngine::ApplyQualityPreset(QualityPreset preset)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    switch (preset)
    {
    case QualityPreset::Low:
        m_settings.maxTextureSize = 512;
        m_settings.shadowMapSize = 512;
        m_settings.msaaSamples = 1;
        m_settings.bloom = false;
        m_settings.ssao = false;
        m_settings.taa = false;
        break;
    case QualityPreset::Medium:
        m_settings.maxTextureSize = 1024;
        m_settings.shadowMapSize = 1024;
        m_settings.msaaSamples = 2;
        m_settings.bloom = true;
        m_settings.ssao = false;
        m_settings.taa = false;
        break;
    case QualityPreset::High:
        m_settings.maxTextureSize = 2048;
        m_settings.shadowMapSize = 2048;
        m_settings.msaaSamples = 4;
        m_settings.bloom = true;
        m_settings.ssao = true;
        m_settings.taa = false;
        break;
    case QualityPreset::Ultra:
        m_settings.maxTextureSize = 4096;
        m_settings.shadowMapSize = 4096;
        m_settings.msaaSamples = 8;
        m_settings.bloom = true;
        m_settings.ssao = true;
        m_settings.taa = true;
        break;
    case QualityPreset::Custom:
        break;
    }

    NotifyStateChange();
}

void GraphicsEngine::NotifyStateChange()
{
    if (m_stateCallback)
        m_stateCallback();
}

// ============================================================================
// Settings — Linux/RHI
// ============================================================================

void GraphicsEngine::SetRenderingPipeline(RenderingPipeline pipeline)
{
    m_currentPipeline = pipeline;
    m_settings.renderPath = pipeline;
}

void GraphicsEngine::SetGraphicsSettings(const GraphicsSettings& settings)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_settings = settings;
}

void GraphicsEngine::SetQualityPreset(QualityPreset preset)
{
    m_settings.qualityPreset = preset;
    switch (preset)
    {
    case QualityPreset::Low:
        m_settings.maxTextureSize = 512;
        m_settings.shadowMapSize = 512;
        m_settings.msaaSamples = 1;
        m_settings.bloom = false;
        m_settings.ssao = false;
        break;
    case QualityPreset::Medium:
        m_settings.maxTextureSize = 1024;
        m_settings.shadowMapSize = 1024;
        m_settings.msaaSamples = 2;
        m_settings.bloom = true;
        m_settings.ssao = false;
        break;
    case QualityPreset::High:
        m_settings.maxTextureSize = 2048;
        m_settings.shadowMapSize = 2048;
        m_settings.msaaSamples = 4;
        m_settings.bloom = true;
        m_settings.ssao = true;
        break;
    case QualityPreset::Ultra:
        m_settings.maxTextureSize = 4096;
        m_settings.shadowMapSize = 4096;
        m_settings.msaaSamples = 8;
        m_settings.bloom = true;
        m_settings.ssao = true;
        m_settings.taa = true;
        break;
    case QualityPreset::Custom:
        break;
    }
}

void GraphicsEngine::SetRenderPath(RenderPath path)
{
    m_currentPipeline = path;
    m_settings.renderPath = path;
}

void GraphicsEngine::SetHDREnabled(bool enabled)
{
    m_hdrEnabled = enabled;
    m_settings.hdr = enabled;
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

// ============================================================================
// Resize and Statistics — Linux/RHI
// ============================================================================

void GraphicsEngine::OnResize(unsigned int width, unsigned int height)
{
    Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void GraphicsEngine::ResetStatistics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_statistics = RenderStatistics{};
}

HRESULT GraphicsEngine::SaveScreenshot(const std::string& filename)
{
#if defined(SPARK_OPENGL_SUPPORT) && SPARK_HAS_STB_IMAGE
    auto& rhi = GetRHI();
    if (!rhi.initialized)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot: RHI not initialized");
        return E_FAIL;
    }

    // Only supported when using the OpenGL backend
    auto* device = rhi.bridge.GetDevice();
    if (!device || device->GetBackendType() != Spark::RHI::GraphicsBackend::OpenGL)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot: only supported with OpenGL backend on Linux");
        return E_NOTIMPL;
    }

    auto* swapChain = rhi.bridge.GetSwapChain();
    if (!swapChain)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot: no swap chain available");
        return E_FAIL;
    }

    const int w = static_cast<int>(swapChain->GetWidth());
    const int h = static_cast<int>(swapChain->GetHeight());
    if (w <= 0 || h <= 0)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot: invalid dimensions %dx%d", w, h);
        return E_FAIL;
    }

    constexpr int channels = 4; // RGBA
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * channels);

    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL reads bottom-up; flip rows so the image is top-down
    const size_t rowBytes = static_cast<size_t>(w) * channels;
    std::vector<uint8_t> rowTemp(rowBytes);
    for (int y = 0; y < h / 2; ++y)
    {
        uint8_t* top = pixels.data() + y * rowBytes;
        uint8_t* bot = pixels.data() + (h - 1 - y) * rowBytes;
        std::copy_n(top, rowBytes, rowTemp.data());
        std::copy_n(bot, rowBytes, top);
        std::copy_n(rowTemp.data(), rowBytes, bot);
    }

    // Choose format based on file extension (default to PNG)
    const std::string& name = filename.empty() ? "screenshot.png" : filename;
    int result = 0;

    if (name.size() >= 4 && name.substr(name.size() - 4) == ".bmp")
    {
        result = stbi_write_bmp(name.c_str(), w, h, channels, pixels.data());
    }
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".tga")
    {
        result = stbi_write_tga(name.c_str(), w, h, channels, pixels.data());
    }
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".jpg")
    {
        result = stbi_write_jpg(name.c_str(), w, h, channels, pixels.data(), 90);
    }
    else
    {
        result = stbi_write_png(name.c_str(), w, h, channels, pixels.data(), static_cast<int>(rowBytes));
    }

    if (result)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Screenshot saved: %s (%dx%d)", name.c_str(), w, h);
        return S_OK;
    }

    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot: failed to write %s", name.c_str());
    return E_FAIL;
#else
    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "SaveScreenshot not available (requires OpenGL + stb_image_write): %s",
                   filename.c_str());
    return E_NOTIMPL;
#endif
}


#endif // !SPARK_PLATFORM_WINDOWS
