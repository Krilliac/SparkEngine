/**
 * @file TextureSystemWindows.cpp
 * @brief D3D11 GPU texture loading, creation, and binding
 */

#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

using namespace DirectX;

// ============================================================================
// TEXTURE CLASS IMPLEMENTATION
// ============================================================================

Texture::Texture(const std::string& name, const TextureDesc& desc) : m_name(name), m_desc(desc) {}

HRESULT Texture::CreateFromFile(const std::string& filePath, ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    // Load image using WIC — ComPtr handles Release() on all exit paths
    HRESULT hr = S_OK;
    ComPtr<IWICImagingFactory> pFactory;
    ComPtr<IWICBitmapDecoder> pDecoder;
    ComPtr<IWICBitmapFrameDecode> pFrame;
    ComPtr<IWICFormatConverter> pConverter;
    UINT width, height;

    // Create WIC factory
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr))
        return hr;

    // Create decoder
    hr = pFactory->CreateDecoderFromFilename(std::wstring(filePath.begin(), filePath.end()).c_str(), nullptr,
                                             GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr))
        return hr;

    // Get frame
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr))
        return hr;

    pFrame->GetSize(&width, &height);
    m_desc.width = width;
    m_desc.height = height;

    // Convert to RGBA
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr))
        return hr;

    hr = pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr))
        return hr;

    // Validate dimensions before allocation to prevent integer overflow
    if (width == 0 || height == 0)
        return E_INVALIDARG;

    // Check for multiplication overflow: width * 4 * height
    constexpr UINT kMaxTextureBytes = 256u * 1024u * 1024u; // 256 MB sanity limit
    if (width > kMaxTextureBytes / 4 || (width * 4) > kMaxTextureBytes / height)
        return E_OUTOFMEMORY;

    // Read pixel data
    UINT stride = width * 4;
    UINT bufferSize = stride * height;
    std::vector<BYTE> buffer(bufferSize);

    hr = pConverter->CopyPixels(nullptr, stride, bufferSize, buffer.data());
    if (SUCCEEDED(hr))
    {
        hr = CreateFromData(buffer.data(), bufferSize, device);
    }

    if (SUCCEEDED(hr))
    {
        m_loaded = true;
        m_memoryUsage = bufferSize;
    }

    return hr;
}

HRESULT Texture::CreateFromData(const void* data, size_t dataSize, ID3D11Device* device)
{
    if (!device || !data || dataSize == 0)
        return E_INVALIDARG;

    if (m_desc.width == 0 || m_desc.height == 0)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data;
    initData.SysMemPitch = m_desc.width * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = CreateViews(device);
        m_loaded = true;
        m_memoryUsage = dataSize;
    }

    return hr;
}

HRESULT Texture::CreateRenderTarget(ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        // Create render target view
        hr = device->CreateRenderTargetView(m_texture.Get(), nullptr, m_rtv.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = CreateViews(device);
            m_loaded = true;
            m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
        }
    }

    return hr;
}

HRESULT Texture::CreateDepthStencil(ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = device->CreateDepthStencilView(m_texture.Get(), nullptr, m_dsv.GetAddressOf());
        m_loaded = true;
        m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    }

    return hr;
}

void Texture::Release()
{
    m_srv.Reset();
    m_uav.Reset();
    m_rtv.Reset();
    m_dsv.Reset();
    m_texture.Reset();
    m_loaded = false;
    m_memoryUsage = 0;
}

void Texture::Bind(ID3D11DeviceContext* context, uint32_t slot)
{
    if (context && m_srv)
    {
        ID3D11ShaderResourceView* srv = m_srv.Get();
        context->PSSetShaderResources(slot, 1, &srv);
    }
}

void Texture::UnBind(ID3D11DeviceContext* context, uint32_t slot)
{
    if (context)
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(slot, 1, &nullSRV);
    }
}

HRESULT Texture::CreateViews(ID3D11Device* device)
{
    HRESULT hr = S_OK;

    // Create Shader Resource View
    D3D11_TEXTURE2D_DESC texDesc;
    m_texture->GetDesc(&texDesc);

    if (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;

        if (texDesc.ArraySize > 1)
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = texDesc.MipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
        }
        else
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        }

        hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    // Create Unordered Access View if requested
    if (texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = texDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        hr = device->CreateUnorderedAccessView(m_texture.Get(), &uavDesc, m_uav.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    return hr;
}

DXGI_FORMAT Texture::GetDXGIFormat(TextureFormat format) const
{
    switch (format)
    {
    case TextureFormat::R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::BC1_UNORM:
        return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::BC1_SRGB:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case TextureFormat::BC3_UNORM:
        return DXGI_FORMAT_BC3_UNORM;
    case TextureFormat::BC3_SRGB:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case TextureFormat::BC7_UNORM:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::BC7_SRGB:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    case TextureFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case TextureFormat::R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// ============================================================================
// TEXTURE SYSTEM — CORE MANAGEMENT
// ============================================================================

TextureSystem::TextureSystem() : m_device(nullptr), m_context(nullptr) {}

TextureSystem::~TextureSystem()
{
    Shutdown();
}

HRESULT TextureSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;

    // Initialize metrics
    memset(&m_metrics, 0, sizeof(m_metrics));

    // Create default textures
    HRESULT hr = CreateDefaultTextures();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create default textures");
        return hr;
    }

    // Start streaming threads
    SetStreamingThreadCount(2);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TextureSystem initialized successfully");
    return S_OK;
}

void TextureSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TextureSystem shutting down");
    // Stop streaming threads
    m_shouldStop = true;
    m_streamingCondition.notify_all();

    for (auto& thread : m_streamingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    // Clear all textures
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures.clear();
    }

    m_whiteTexture.reset();
    m_blackTexture.reset();
    m_normalTexture.reset();
    m_noiseTexture.reset();

    m_device = nullptr;
    m_context = nullptr;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TextureSystem shutdown complete");
}

std::shared_ptr<Texture> TextureSystem::LoadTexture(const std::string& filePath, const TextureDesc& desc)
{
    ASSERT_MSG(!filePath.empty(), "TextureSystem::LoadTexture — filePath must not be empty");
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        auto it = m_textures.find(filePath);
        if (it != m_textures.end())
        {
            // Update LRU data on access
            auto& lru = m_lruData[filePath];
            lru.lastUsedFrame = m_currentFrame;
            lru.lastUsedTime = std::chrono::steady_clock::now();
            return it->second;
        }
    }

    // Adjust description for quality settings
    TextureDesc adjustedDesc = AdjustDescForQuality(desc);

    // Load the texture
    auto texture = LoadTextureFromFile(filePath, adjustedDesc);
    if (texture)
    {
        {
            std::lock_guard<std::mutex> lock(m_texturesMutex);
            m_textures[filePath] = texture;

            // Initialize LRU data for newly loaded texture
            auto& lru = m_lruData[filePath];
            lru.lastUsedFrame = m_currentFrame;
            lru.lastUsedTime = std::chrono::steady_clock::now();
            lru.priority = 2; // Normal priority
        }
        {
            std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
            m_metrics.loadedTextures++;
        }
    }

    return texture;
}

std::shared_ptr<Texture> TextureSystem::CreateTexture(const std::string& name, const TextureDesc& desc)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::CreateTexture — name must not be empty");
    ASSERT_MSG(desc.width > 0 && desc.height > 0, "TextureSystem::CreateTexture — dimensions must be positive");
    auto texture = std::make_shared<Texture>(name, desc);

    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures[name] = texture;
    }

    return texture;
}

std::shared_ptr<Texture> TextureSystem::GetTexture(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_textures.find(name);
    return (it != m_textures.end()) ? it->second : nullptr;
}

void TextureSystem::UnloadTexture(const std::string& name)
{
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        auto it = m_textures.find(name);
        if (it != m_textures.end())
        {
            m_textures.erase(it);
            erased = true;
        }
    }
    if (erased)
    {
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        if (m_metrics.loadedTextures > 0)
            m_metrics.loadedTextures--;
    }
}

void TextureSystem::UnloadAllTextures()
{
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures.clear();
    }
    {
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedTextures = 0;
    }
}

size_t TextureSystem::GetMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    size_t totalMemory = 0;

    for (const auto& pair : m_textures)
    {
        totalMemory += pair.second->GetMemoryUsage();
    }

    return totalMemory;
}

// Console integration methods
TextureSystem::TextureMetrics TextureSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string TextureSystem::Console_ListTextures() const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    std::stringstream ss;

    ss << "=== Loaded Textures (" << m_textures.size() << ") ===\n";
    for (const auto& pair : m_textures)
    {
        const auto& texture = pair.second;
        ss << pair.first << " - " << texture->GetDesc().width << "x" << texture->GetDesc().height;
        ss << " (" << (texture->GetMemoryUsage() / 1024) << " KB)\n";
    }

    return ss.str();
}

std::string TextureSystem::Console_GetTextureInfo(const std::string& name) const
{
    auto texture = GetTexture(name);
    if (!texture)
    {
        return "Texture not found: " + name;
    }

    const auto& desc = texture->GetDesc();
    std::stringstream ss;

    ss << "=== Texture Info: " << name << " ===\n";
    ss << "Dimensions: " << desc.width << "x" << desc.height << "x" << desc.depth << "\n";
    ss << "Mip Levels: " << desc.mipLevels << "\n";
    ss << "Array Size: " << desc.arraySize << "\n";
    ss << "Memory Usage: " << (texture->GetMemoryUsage() / 1024) << " KB\n";
    ss << "Loaded: " << (texture->IsLoaded() ? "Yes" : "No") << "\n";
    ss << "Streaming: " << (texture->IsStreaming() ? "Yes" : "No") << "\n";

    return ss.str();
}

void TextureSystem::Console_SetQuality(const std::string& quality)
{
    if (quality == "low")
    {
        SetTextureQuality(TextureQuality::Low);
    }
    else if (quality == "medium")
    {
        SetTextureQuality(TextureQuality::Medium);
    }
    else if (quality == "high")
    {
        SetTextureQuality(TextureQuality::High);
    }
    else if (quality == "ultra")
    {
        SetTextureQuality(TextureQuality::Ultra);
    }

    Spark::SimpleConsole::GetInstance().LogSuccess("Texture quality set to: " + quality);
}

void TextureSystem::Console_SetMemoryBudget(size_t budgetMB)
{
    SetMemoryBudget(budgetMB * 1024 * 1024);
    Spark::SimpleConsole::GetInstance().LogSuccess("Texture memory budget set to: " + std::to_string(budgetMB) + " MB");
}

void TextureSystem::Console_ForceGC()
{
    size_t beforeMemory = GetMemoryUsage();
    GarbageCollect();
    size_t afterMemory = GetMemoryUsage();

    Spark::SimpleConsole::GetInstance().LogSuccess(
        "Garbage collection freed: " + std::to_string((beforeMemory - afterMemory) / 1024) + " KB");
}

void TextureSystem::Console_ReloadTexture(const std::string& name)
{
    UnloadTexture(name);
    // Note: Texture will be reloaded on next access
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked texture for reload: " + name);
}

void TextureSystem::Console_ReloadAllTextures()
{
    UnloadAllTextures();
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked all textures for reload");
}

// Private helper methods
HRESULT TextureSystem::CreateDefaultTextures()
{
    // Create white texture (1x1 white pixel)
    {
        uint32_t whitePixel = 0xFFFFFFFF;
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;

        m_whiteTexture = std::make_shared<Texture>("__white", desc);
        HRESULT hr = m_whiteTexture->CreateFromData(&whitePixel, 4, m_device);
        if (FAILED(hr))
            return hr;
    }

    // Create black texture (1x1 black pixel)
    {
        uint32_t blackPixel = 0xFF000000;
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;

        m_blackTexture = std::make_shared<Texture>("__black", desc);
        HRESULT hr = m_blackTexture->CreateFromData(&blackPixel, 4, m_device);
        if (FAILED(hr))
            return hr;
    }

    // Create default normal texture (1x1 normal pointing up)
    {
        uint32_t normalPixel = 0xFFFF8080; // (0.5, 0.5, 1.0) in normal map format
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;

        m_normalTexture = std::make_shared<Texture>("__normal", desc);
        HRESULT hr = m_normalTexture->CreateFromData(&normalPixel, 4, m_device);
        if (FAILED(hr))
            return hr;
    }

    // Create noise texture (64x64 random noise)
    {
        const int noiseSize = 64;
        std::vector<uint32_t> noiseData(noiseSize * noiseSize);

        for (int i = 0; i < noiseSize * noiseSize; ++i)
        {
            uint8_t r = rand() % 256;
            uint8_t g = rand() % 256;
            uint8_t b = rand() % 256;
            noiseData[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }

        TextureDesc desc;
        desc.width = noiseSize;
        desc.height = noiseSize;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;

        m_noiseTexture = std::make_shared<Texture>("__noise", desc);
        HRESULT hr = m_noiseTexture->CreateFromData(noiseData.data(), noiseData.size() * 4, m_device);
        if (FAILED(hr))
            return hr;
    }

    return S_OK;
}

TextureDesc TextureSystem::AdjustDescForQuality(const TextureDesc& desc) const
{
    TextureDesc adjustedDesc = desc;

    switch (m_quality)
    {
    case TextureQuality::Low:
        adjustedDesc.width = std::max(1u, desc.width / 4);
        adjustedDesc.height = std::max(1u, desc.height / 4);
        break;
    case TextureQuality::Medium:
        adjustedDesc.width = std::max(1u, desc.width / 2);
        adjustedDesc.height = std::max(1u, desc.height / 2);
        break;
    case TextureQuality::High:
    case TextureQuality::Ultra:
    default:
        // No changes for high/ultra quality
        break;
    }

    return adjustedDesc;
}

std::shared_ptr<Texture> TextureSystem::LoadTextureFromFile(const std::string& filePath, const TextureDesc& desc)
{
    if (!std::filesystem::exists(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return nullptr;
    }

    auto texture = std::make_shared<Texture>(filePath, desc);
    HRESULT hr = texture->CreateFromFile(filePath, m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + filePath);
        return nullptr;
    }

    return texture;
}

// Utility functions
TextureFormat GetOptimalFormat(const std::string& filePath, bool sRGB)
{
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".dds")
    {
        return sRGB ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM;
    }
    else if (ext == ".hdr")
    {
        return TextureFormat::R16G16B16A16_FLOAT;
    }
    else
    {
        return sRGB ? TextureFormat::R8G8B8A8_SRGB : TextureFormat::R8G8B8A8_UNORM;
    }
}

bool IsCompressedFormat(TextureFormat format)
{
    return format == TextureFormat::BC1_UNORM || format == TextureFormat::BC1_SRGB ||
           format == TextureFormat::BC3_UNORM || format == TextureFormat::BC3_SRGB ||
           format == TextureFormat::BC7_UNORM || format == TextureFormat::BC7_SRGB;
}

uint32_t GetFormatBytesPerPixel(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::R8G8B8A8_UNORM:
    case TextureFormat::R8G8B8A8_SRGB:
        return 4;
    case TextureFormat::R16G16B16A16_FLOAT:
        return 8;
    case TextureFormat::R32G32B32A32_FLOAT:
        return 16;
    case TextureFormat::R16_FLOAT:
        return 2;
    case TextureFormat::R32_FLOAT:
        return 4;
    default:
        return 4; // Default to 4 bytes per pixel
    }
}

#endif // SPARK_PLATFORM_WINDOWS
