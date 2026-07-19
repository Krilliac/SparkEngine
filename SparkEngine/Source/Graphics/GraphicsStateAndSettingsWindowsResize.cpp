/**
 * @file GraphicsStateAndSettingsWindowsResize.cpp
 * @brief D3D11 resize handling, statistics reset, and screenshot capture
 *
 * Contains OnResize, ResetStatistics, and SaveScreenshot split from
 * GraphicsStateAndSettingsWindows.cpp.
 * Linux counterpart lives in GraphicsStateAndSettingsLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "ScreenSpaceEffects.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#if SPARK_HAS_STB_IMAGE
#include <stb_image_write.h>
#endif

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

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

#endif // SPARK_PLATFORM_WINDOWS
