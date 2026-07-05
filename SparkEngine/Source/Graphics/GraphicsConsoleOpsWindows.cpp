/**
 * @file GraphicsConsoleOpsWindows.cpp
 * @brief Windows/D3D11 implementation — split from GraphicsConsoleOps.cpp
 */
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
#include "../Utils/SparkConsole.h"
#include "ScreenCapture.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <string>
#include <vector>
#include <cstring>
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
    // Real backbuffer readback: swapchain -> staging copy -> map -> PNG via
    // ScreenCapture (which owns naming/output dir when filename is empty).
    LOG_TO_CONSOLE_IMMEDIATE(L"Taking screenshot", L"INFO");

    if (!m_device || !m_context || !m_swapChain)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: no D3D11 device/swapchain", L"ERROR");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: GetBuffer", L"ERROR");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.SampleDesc = {1, 0};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: staging texture", L"ERROR");
        return false;
    }
    m_context->CopyResource(staging.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: map", L"ERROR");
        return false;
    }

    const uint32_t w = desc.Width, h = desc.Height;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = rgba.data() + static_cast<size_t>(y) * w * 4;
        if (bgra)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 0xFF;
            }
        }
        else // RGBA-family formats copy straight through (alpha forced opaque)
        {
            std::memcpy(dst, src, static_cast<size_t>(w) * 4);
            for (uint32_t x = 0; x < w; ++x)
                dst[x * 4 + 3] = 0xFF;
        }
    }
    m_context->Unmap(staging.Get(), 0);

    auto& capture = Spark::Graphics::ScreenCapture::GetInstance();
    if (!capture.IsInitialized())
        capture.Initialize();

    Spark::Graphics::CaptureResult result;
    if (filename.empty())
    {
        result = capture.TakeScreenshot(rgba.data(), w, h);
    }
    else
    {
        result = capture.WriteTo(rgba.data(), w, h, filename);
    }

    if (!result.success)
    {
        const std::wstring werr(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: " + werr, L"ERROR");
        return false;
    }
    const std::wstring wpath(result.filePath.begin(), result.filePath.end());
    LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot saved as " + wpath, L"SUCCESS");
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
    ss << "Average FPS: " << (seconds > 0 ? (frameCount / static_cast<float>(seconds)) : 0.0f) << "\n";
    ss << "Average Frame Time: " << (frameCount > 0 ? (totalFrameTime / frameCount) : 0.0f) << " ms\n";
    ss << "Min Frame Time: " << (frameCount > 0 ? minFrameTime : 0.0f) << " ms\n";
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
        catch (const std::exception& e)
        {
            std::wstring msg = L"Texture system cleanup failed: " + std::wstring(e.what(), e.what() + strlen(e.what()));
            LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Texture system cleanup failed: unknown exception", L"WARNING");
        }
    }

    if (m_assetPipeline)
    {
        try
        {
            m_assetPipeline->Console_ForceGC();
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline garbage collection triggered", L"INFO");
        }
        catch (const std::exception& e)
        {
            std::wstring msg = L"Asset pipeline GC failed: " + std::wstring(e.what(), e.what() + strlen(e.what()));
            LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline GC failed: unknown exception", L"WARNING");
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
        catch (const std::exception& e)
        {
            std::wstring msg = L"VRAM metrics unavailable: " + std::wstring(e.what(), e.what() + strlen(e.what()));
            LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"VRAM metrics unavailable: unknown exception", L"WARNING");
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
    catch (const std::exception& e)
    {
        std::wstring msg = L"Exception during device reset: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"ERROR");
    }
    catch (...)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Unknown exception during device reset", L"ERROR");
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


#endif // SPARK_PLATFORM_WINDOWS
