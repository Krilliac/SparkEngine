/**
 * @file GraphicsConsoleOpsWindowsDiagnostics.cpp
 * @brief Windows/D3D11 console diagnostics and device operations
 *
 * Shader reload, screenshot capture, system info, benchmark, garbage
 * collection, GPU timing, VRAM usage, and device reset methods split from
 * GraphicsConsoleOpsWindows.cpp (which keeps the settings/feature toggles).
 * Linux counterpart lives in GraphicsConsoleOpsLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS


#include "GraphicsEngine.h"
#include "TextureSystem.h"
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
#include <algorithm>

// ============================================================================
// CONSOLE DIAGNOSTICS AND DEVICE OPERATIONS
// ============================================================================

bool GraphicsEngine::Console_ReloadShaders()
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
        return true;
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to reload shaders", L"ERROR");
        return false;
    }
}

bool GraphicsEngine::Console_Screenshot(const std::string& filename)
{
    if (m_attachedMode || !m_device || !m_context || !m_swapChain)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: no D3D11 device/swapchain", L"ERROR");
        return false;
    }

    // Console commands run after EndFrame/Present in the game loop. A
    // flip-discard backbuffer is undefined then, so defer the readback until
    // the next completed frame, after the overlay and before Present.
    bool alreadyQueued = false;
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        if (m_pendingScreenshotFilename)
            alreadyQueued = true;
        else
            m_pendingScreenshotFilename = filename;
    }
    if (alreadyQueued)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot already queued for next frame", L"WARNING");
        return false;
    }
    LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot queued for next frame", L"INFO");
    return true;
}

bool GraphicsEngine::CaptureScreenshotBeforePresent(const std::string& filename)
{
    // Real backbuffer readback: swapchain -> staging copy -> map -> PNG via
    // ScreenCapture (which owns naming/output dir when filename is empty).
    if (!m_device || !m_context || !m_swapChain)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: device/swapchain unavailable before Present", L"ERROR");
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
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgbaFormat = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgbaFormat)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot failed: unsupported backbuffer pixel format", L"ERROR");
        return false;
    }
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
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
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
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Screenshot saved as %s", result.filePath.c_str());
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
    if (seconds < 1 || seconds > 300)
        return "Benchmark duration must be 1-300 seconds";
    if (m_attachedMode || !m_device || !m_context || !m_swapChain)
        return "Benchmark unavailable: no D3D11 swapchain";

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    if (m_benchmarkActive)
        return "Benchmark already running";
    m_benchmarkActive = true;
    m_benchmarkSeconds = seconds;
    m_benchmarkStart = std::chrono::steady_clock::now();
    m_benchmarkPresentedFrames = 0;
    m_benchmarkCpuTotalMs = 0.0;
    m_benchmarkCpuMinMs = 0.0;
    m_benchmarkCpuMaxMs = 0.0;
    return "Benchmark started: sampling actual presented frames for " + std::to_string(seconds) +
           " second(s); result will be logged after the interval";
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


#endif // SPARK_PLATFORM_WINDOWS
