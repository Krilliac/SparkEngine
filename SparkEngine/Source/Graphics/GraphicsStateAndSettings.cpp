/**
 * @file GraphicsStateAndSettings.cpp
 * @brief Graphics state management, metrics, settings, and resize for GraphicsEngine
 *
 * Contains ApplyGraphicsState, UpdateMetrics, UpdateAdvancedMetrics, quality presets,
 * render path/pipeline settings, MSAA/TAA/SSAO/SSR/Volumetric settings, OnResize,
 * and statistics/screenshot methods. Split from GraphicsEngine.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "ScreenSpaceEffects.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <DirectXMath.h>
#include <wrl.h>

#if SPARK_HAS_STB_IMAGE
#include <stb_image_write.h>
#endif

#include <string>
#include <chrono>
#include <cstring>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================================
// GRAPHICS STATE AND METRICS
// ============================================================================

void GraphicsEngine::ApplyGraphicsState()
{
    if (m_settings.wireframeMode && m_wireframeRasterState)
    {
        m_context->RSSetState(m_wireframeRasterState.Get());
    }
    else if (m_solidRasterState)
    {
        m_context->RSSetState(m_solidRasterState.Get());
    }
}

void GraphicsEngine::UpdateMetrics()
{
    static int frameCount = 0;
    frameCount++;

    auto now = std::chrono::high_resolution_clock::now();
    static auto lastUpdate = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);

    if (elapsed.count() >= 1000)
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);

        m_statistics.fps = static_cast<uint32_t>(frameCount * 1000.0f / elapsed.count());
        frameCount = 0;
        lastUpdate = now;

        m_statistics.vsyncEnabled = m_settings.vsync;
        m_statistics.wireframeMode = m_settings.wireframeMode;
        m_statistics.debugMode = m_settings.debugMode;
        m_statistics.bufferMemory = m_bufferMemoryUsage;
        m_statistics.totalGPUMemory = m_textureMemoryUsage + m_bufferMemoryUsage;
    }

    UpdateAdvancedMetrics();
}

void GraphicsEngine::UpdateAdvancedMetrics()
{
    auto now = std::chrono::high_resolution_clock::now();
    static auto lastAdvancedUpdate = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAdvancedUpdate);

    if (elapsed.count() >= 500)
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);

        // Update texture system metrics
        if (m_textureSystem)
        {
            try
            {
                auto textureMetrics = m_textureSystem->Console_GetMetrics();
                m_statistics.textureMemory = textureMetrics.totalMemoryUsage;
                m_statistics.textureBinds = textureMetrics.textureBinds;
            }
            catch (const std::exception& e)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: TextureSystem metrics unavailable: " +
                                             std::wstring(e.what(), e.what() + strlen(e.what())),
                                         L"WARNING");
                m_statistics.textureMemory = m_textureMemoryUsage;
                m_statistics.textureBinds = 50;
            }
            catch (...)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: TextureSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.textureMemory = m_textureMemoryUsage;
                m_statistics.textureBinds = 50;
            }
        }
        else
        {
            m_statistics.textureMemory = m_textureMemoryUsage;
            m_statistics.textureBinds = 0;
        }

        // Update material system metrics
        if (m_materialSystem)
        {
            try
            {
                auto materialMetrics = m_materialSystem->Console_GetMetrics();
                m_statistics.materialSwitches = materialMetrics.materialSwitches;
            }
            catch (const std::exception& e)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: MaterialSystem metricsUnavailable: " +
                                             std::wstring(e.what(), e.what() + strlen(e.what())),
                                         L"WARNING");
                m_statistics.materialSwitches = 10;
            }
            catch (...)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: MaterialSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.materialSwitches = 10;
            }
        }
        else
        {
            m_statistics.materialSwitches = 0;
        }

        // Update lighting system metrics
        if (m_lightingSystem)
        {
            try
            {
                auto lightingMetrics = m_lightingSystem->Console_GetMetrics();
                m_statistics.activeLights = lightingMetrics.activeLights;
                m_statistics.shadowUpdates = lightingMetrics.shadowMapUpdates;
                m_statistics.lightCullingTime = lightingMetrics.lightCullingTime;
            }
            catch (const std::exception& e)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem metrics unavailable: " +
                                             std::wstring(e.what(), e.what() + strlen(e.what())),
                                         L"WARNING");
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = 1;
                m_statistics.lightCullingTime = 0.5f;
            }
            catch (...)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = 1;
                m_statistics.lightCullingTime = 0.5f;
            }
        }
        else
        {
            m_statistics.activeLights = 0;
            m_statistics.shadowUpdates = 0;
            m_statistics.lightCullingTime = 0.0f;
        }

        // Update post-processing metrics
        if (m_postProcessing)
        {
            m_statistics.postProcessPasses = 0;
            if (m_settings.bloom)
                m_statistics.postProcessPasses++;
            if (m_settings.ssao && m_ssaoSettings.enabled)
                m_statistics.postProcessPasses++;
            if (m_settings.taa && m_taaSettings.enabled)
                m_statistics.postProcessPasses++;
            if (m_hdrEnabled)
                m_statistics.postProcessPasses++;
        }
        else
        {
            m_statistics.postProcessPasses = 0;
        }

        // Update memory metrics
        m_statistics.meshMemory = m_bufferMemoryUsage;
        m_statistics.totalGPUMemory = m_statistics.textureMemory + m_statistics.meshMemory;

        lastAdvancedUpdate = now;
    }
}

void GraphicsEngine::ApplyAdvancedGraphicsState()
{
    if (m_taaSettings.enabled)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Applying TAA settings", L"INFO");
    }

    if (m_ssaoSettings.enabled && m_postProcessing)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Applying SSAO settings", L"INFO");
    }

    if (m_lightingSystem)
    {
        m_lightingSystem->EnableShadows(m_settings.shadows);
        m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
    }

    if (m_materialSystem)
    {
        std::string quality = "high";
        if (m_settings.maxTextureSize <= 512)
            quality = "low";
        else if (m_settings.maxTextureSize <= 1024)
            quality = "medium";
        else if (m_settings.maxTextureSize <= 2048)
            quality = "high";
        else
            quality = "ultra";

        m_materialSystem->Console_SetTextureQuality(quality);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Advanced graphics state applied", L"INFO");
}

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

// ============================================================================
// RESIZE AND STATISTICS
// ============================================================================

void GraphicsEngine::OnResize(unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0)
        return;
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(
            L"Handling window resize to " + std::to_wstring(width) + L"x" + std::to_wstring(height), L"INFO");
    }

    m_windowWidth = width;
    m_windowHeight = height;

    if (m_context)
    {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Device context not available during resize", L"WARNING");
    }
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    if (m_swapChain)
    {
        HRESULT resizeHr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeHr))
        {
            SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "Graphics", 2,
                                    "SwapChain::ResizeBuffers failed HR=0x%08lX for size %ux%u",
                                    static_cast<long>(resizeHr), width, height);
        }
    }
    else
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Warn, "Graphics", 2, "Swap chain not available during resize to %ux%u",
                                width, height);
    }

    if (!m_device)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Device not available during resize", L"ERROR");
        return;
    }

    CreateRenderTargetView();
    CreateDepthStencilView();
    SetViewport();
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();

    // Resize post-processing, temporal effects, and screen-space effects
    if (m_postProcessing)
        m_postProcessing->Resize(width, height);
    if (m_temporalEffects)
        m_temporalEffects->Resize(width, height);
    if (m_screenSpaceEffects)
        m_screenSpaceEffects->Resize(width, height);
}

void GraphicsEngine::ResetStatistics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_statistics = RenderStatistics{};
}

HRESULT GraphicsEngine::SaveScreenshot(const std::string& filename)
{
#if SPARK_HAS_STB_IMAGE
    if (!m_swapChain || !m_device || !m_context)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot: device not initialized", L"WARNING");
        return E_FAIL;
    }

    // Get the back buffer texture from the swap chain
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot: failed to get back buffer", L"WARNING");
        return hr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    // Create a staging texture for CPU readback
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;

    ComPtr<ID3D11Texture2D> staging;
    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot: failed to create staging texture", L"WARNING");
        return hr;
    }

    m_context->CopyResource(staging.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot: failed to map staging texture", L"WARNING");
        return hr;
    }

    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    constexpr int channels = 4; // RGBA
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * channels);

    // Copy row by row (mapped pitch may differ from image width)
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < h; ++y)
    {
        const auto* srcRow = src + y * mapped.RowPitch;
        auto* dstRow = pixels.data() + y * w * channels;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * channels);
    }

    m_context->Unmap(staging.Get(), 0);

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
        const int stride = w * channels;
        result = stbi_write_png(name.c_str(), w, h, channels, pixels.data(), stride);
    }

    if (result)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot saved: " + std::wstring(name.begin(), name.end()), L"INFO");
        return S_OK;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot: failed to write " + std::wstring(name.begin(), name.end()), L"WARNING");
    return E_FAIL;
#else
    LOG_TO_CONSOLE_IMMEDIATE(L"SaveScreenshot not available (requires stb_image_write): " +
                                 std::wstring(filename.begin(), filename.end()),
                             L"WARNING");
    return E_NOTIMPL;
#endif
}

#else // !SPARK_PLATFORM_WINDOWS

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

#endif // SPARK_PLATFORM_WINDOWS
