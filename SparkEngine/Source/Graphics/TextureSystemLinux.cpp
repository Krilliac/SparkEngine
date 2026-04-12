/**
 * @file TextureSystemLinux.cpp
 * @brief CPU-portable texture system logic (metadata, caching, format detection)
 */

#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include "../Utils/Validate.h"
#include "../Utils/LogMacros.h"
#include <sstream>
#include <algorithm>
#include <filesystem>

#if SPARK_HAS_STB_IMAGE
#include <stb_image.h>
#endif

#if SPARK_HAS_TINYEXR
#include <tinyexr.h>
#endif

// ============================================================================
// Texture class (Linux — stb_image/tinyexr for real image loading)
// ============================================================================

Texture::Texture(const std::string& name, const TextureDesc& desc) : m_name(name), m_desc(desc) {}

HRESULT Texture::CreateFromFile(const std::string& filePath, ID3D11Device* /*device*/)
{
#if SPARK_HAS_TINYEXR
    // Check for EXR format — tinyexr handles OpenEXR natively
    {
        std::string ext = std::filesystem::path(filePath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".exr")
        {
            float* rgba = nullptr;
            int width = 0, height = 0;
            const char* err = nullptr;
            int ret = LoadEXR(&rgba, &width, &height, filePath.c_str(), &err);
            if (ret == TINYEXR_SUCCESS && rgba)
            {
                m_desc.width = static_cast<uint32_t>(width);
                m_desc.height = static_cast<uint32_t>(height);
                m_desc.format = TextureFormat::R32G32B32A32_FLOAT;
                m_memoryUsage = static_cast<size_t>(width * height * 16);
                m_loaded = true;
                free(rgba);
                return S_OK;
            }
            if (err)
            {
                fprintf(stderr, "[TextureSystem] tinyexr failed to load: %s (%s)\n", filePath.c_str(), err);
                FreeEXRErrorMessage(err);
            }
        }
    }
#endif // SPARK_HAS_TINYEXR

#if SPARK_HAS_STB_IMAGE
    // Check for HDR format first (stbi_loadf returns float data)
    if (stbi_is_hdr(filePath.c_str()))
    {
        int width = 0, height = 0, channels = 0;
        float* hdrPixels = stbi_loadf(filePath.c_str(), &width, &height, &channels, 4);
        if (hdrPixels)
        {
            m_desc.width = static_cast<uint32_t>(width);
            m_desc.height = static_cast<uint32_t>(height);
            m_desc.format = TextureFormat::R32G32B32A32_FLOAT;
            m_memoryUsage = static_cast<size_t>(width * height * 16); // 4 floats per pixel
            m_loaded = true;
            stbi_image_free(hdrPixels);
            return S_OK;
        }
    }

    // Standard LDR image loading (PNG, JPG, BMP, TGA)
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(filePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels)
    {
        m_desc.width = static_cast<uint32_t>(width);
        m_desc.height = static_cast<uint32_t>(height);
        m_memoryUsage = static_cast<size_t>(width * height * 4);
        m_loaded = true;
        stbi_image_free(pixels);
        return S_OK;
    }
    fprintf(stderr, "[TextureSystem] stb_image failed to load: %s (%s)\n", filePath.c_str(), stbi_failure_reason());
#endif
    // Fallback: mark as loaded with estimated size
    m_loaded = true;
    m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    return S_OK;
}

HRESULT Texture::CreateFromData(const void* data, size_t dataSize, ID3D11Device* /*device*/)
{
#if SPARK_HAS_STB_IMAGE
    // If data looks like an encoded image (not raw pixel data), try decoding it
    if (data && dataSize > 4)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        // Check for common image signatures: PNG, JPEG, BMP
        bool isEncodedImage = (bytes[0] == 0x89 && bytes[1] == 'P') ||  // PNG
                              (bytes[0] == 0xFF && bytes[1] == 0xD8) || // JPEG
                              (bytes[0] == 'B' && bytes[1] == 'M');     // BMP
        if (isEncodedImage)
        {
            int width = 0, height = 0, channels = 0;
            stbi_uc* pixels =
                stbi_load_from_memory(bytes, static_cast<int>(dataSize), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels)
            {
                m_desc.width = static_cast<uint32_t>(width);
                m_desc.height = static_cast<uint32_t>(height);
                m_memoryUsage = static_cast<size_t>(width * height * 4);
                m_loaded = true;
                stbi_image_free(pixels);
                return S_OK;
            }
        }
    }
#endif
    m_loaded = true;
    m_memoryUsage = dataSize;
    return S_OK;
}

HRESULT Texture::CreateRenderTarget(ID3D11Device* /*device*/)
{
    m_loaded = true;
    m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    return S_OK;
}

HRESULT Texture::CreateDepthStencil(ID3D11Device* /*device*/)
{
    m_loaded = true;
    m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    return S_OK;
}

void Texture::Release()
{
    m_loaded = false;
    m_memoryUsage = 0;
}

void Texture::Bind(ID3D11DeviceContext* /*context*/, uint32_t /*slot*/)
{
    // No-op on Linux
}

void Texture::UnBind(ID3D11DeviceContext* /*context*/, uint32_t /*slot*/)
{
    // No-op on Linux
}

HRESULT Texture::CreateViews(ID3D11Device* /*device*/)
{
    return S_OK;
}

DXGI_FORMAT Texture::GetDXGIFormat(TextureFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}

// ============================================================================
// TextureSystem — Core Management (Linux stub)
// ============================================================================

TextureSystem::TextureSystem() : m_device(nullptr), m_context(nullptr)
{
    memset(&m_metrics, 0, sizeof(m_metrics));
}

TextureSystem::~TextureSystem()
{
    Shutdown();
}

HRESULT TextureSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT_NOT_NULL(device);
    ASSERT_NOT_NULL(context);
    m_device = device;
    m_context = context;
    memset(&m_metrics, 0, sizeof(m_metrics));

    // Create default textures (CPU-side only)
    {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        m_whiteTexture = std::make_shared<Texture>("__white", desc);
        HRESULT hr = m_whiteTexture->CreateFromData(nullptr, 4, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "TextureSystem: default white texture CreateFromData failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        m_blackTexture = std::make_shared<Texture>("__black", desc);
        HRESULT hr = m_blackTexture->CreateFromData(nullptr, 4, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "TextureSystem: default black texture CreateFromData failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        m_normalTexture = std::make_shared<Texture>("__normal", desc);
        HRESULT hr = m_normalTexture->CreateFromData(nullptr, 4, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "TextureSystem: default normal texture CreateFromData failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    {
        TextureDesc desc;
        desc.width = 64;
        desc.height = 64;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        m_noiseTexture = std::make_shared<Texture>("__noise", desc);
        HRESULT hr = m_noiseTexture->CreateFromData(nullptr, 64 * 64 * 4, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "TextureSystem: default noise texture CreateFromData failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }

    return S_OK;
}

void TextureSystem::Shutdown()
{
    m_shouldStop = true;
    m_streamingCondition.notify_all();

    for (auto& thread : m_streamingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_streamingThreads.clear();

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
}

std::shared_ptr<Texture> TextureSystem::LoadTexture(const std::string& filePath, const TextureDesc& desc)
{
    ASSERT_MSG(!filePath.empty(), "TextureSystem::LoadTexture — filePath must not be empty");
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

    TextureDesc adjustedDesc = AdjustDescForQuality(desc);
    auto texture = std::make_shared<Texture>(filePath, adjustedDesc);
    texture->CreateFromFile(filePath, nullptr);

    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures[filePath] = texture;

        // Initialize LRU data
        auto& lru = m_lruData[filePath];
        lru.lastUsedFrame = m_currentFrame;
        lru.lastUsedTime = std::chrono::steady_clock::now();
        lru.priority = 2;
    }
    {
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedTextures++;
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
        SetTextureQuality(TextureQuality::Low);
    else if (quality == "medium")
        SetTextureQuality(TextureQuality::Medium);
    else if (quality == "high")
        SetTextureQuality(TextureQuality::High);
    else if (quality == "ultra")
        SetTextureQuality(TextureQuality::Ultra);
}

void TextureSystem::Console_SetMemoryBudget(size_t budgetMB)
{
    SetMemoryBudget(budgetMB * 1024 * 1024);
}

void TextureSystem::Console_ForceGC()
{
    GarbageCollect();
}

void TextureSystem::Console_ReloadTexture(const std::string& name)
{
    UnloadTexture(name);
}

void TextureSystem::Console_ReloadAllTextures()
{
    UnloadAllTextures();
}

HRESULT TextureSystem::CreateDefaultTextures()
{
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
        break;
    }
    return adjustedDesc;
}

std::shared_ptr<Texture> TextureSystem::LoadTextureFromFile(const std::string& filePath, const TextureDesc& desc)
{
    auto texture = std::make_shared<Texture>(filePath, desc);
    texture->CreateFromFile(filePath, nullptr);
    return texture;
}

bool TextureSystem::IsTextureFormatSupported(TextureFormat /*format*/) const
{
    return true;
}

uint32_t TextureSystem::CalculateMemoryUsage(const TextureDesc& desc) const
{
    return desc.width * desc.height * 4 * desc.arraySize;
}

// Utility functions
TextureFormat GetOptimalFormat(const std::string& filePath, bool sRGB)
{
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".dds")
        return sRGB ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM;
    else if (ext == ".hdr" || ext == ".exr")
        return TextureFormat::R16G16B16A16_FLOAT;
    else
        return sRGB ? TextureFormat::R8G8B8A8_SRGB : TextureFormat::R8G8B8A8_UNORM;
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
        return 4;
    }
}

#endif // !SPARK_PLATFORM_WINDOWS
