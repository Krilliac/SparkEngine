/**
 * @file AssetTypesLinuxMedia.cpp
 * @brief Linux texture/audio asset implementations (TextureAsset, AudioAsset) and AssetCache
 *
 * Uses stb_image for texture decoding and the RHI bridge for GPU upload,
 * without D3D11 dependencies. Split from AssetTypesLinux.cpp, which keeps
 * the MeshAsset loaders. The Windows counterpart lives in AssetTypesWindows.cpp.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include "Utils/LogMacros.h"
#include <chrono>
#include <filesystem>
#include <fstream>

#if SPARK_HAS_STB_IMAGE
#include <stb_image.h>
#endif

// ============================================================================
// Texture/audio asset implementations (Linux)
// ============================================================================

HRESULT TextureAsset::Load(ID3D11Device* /*device*/)
{
    m_metadata.filePath = m_path;
    m_metadata.name = std::filesystem::path(m_path).stem().string();
    m_metadata.type = AssetType::Texture;
    m_metadata.state = StreamingState::Loaded;
    if (std::filesystem::exists(m_path))
    {
        m_metadata.fileSize = std::filesystem::file_size(m_path);

#if SPARK_HAS_STB_IMAGE
        // Decode the image into a CPU-side RGBA8 buffer, then hand that to
        // the RHI bridge as a textured surface. stb_image converts any
        // supported format (PNG, JPG, TGA, BMP, HDR...) to 4-channel 8-bit.
        // Linux / macOS get the full texture pipeline this way without
        // needing WIC (Windows) or a platform-specific image library.
        int iw = 0;
        int ih = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(m_path.c_str(), &iw, &ih, &channels, STBI_rgb_alpha);
        if (pixels && iw > 0 && ih > 0)
        {
            m_width = static_cast<uint32_t>(iw);
            m_height = static_cast<uint32_t>(ih);

            auto& rhi = Spark::Graphics::Detail::GetRHI();
            if (rhi.initialized)
            {
                // CreateTexture2D takes RGBA8 pixel data directly. NullRHI
                // stubs the upload but returns a valid handle so the bind
                // path in ModelLoading.cpp still runs on headless builds.
                auto tex = rhi.bridge.CreateTexture2D(m_width, m_height, Spark::RHI::PixelFormat::R8G8B8A8_UNORM,
                                                      Spark::RHI::RHITextureUsage::ShaderResource, pixels);
                if (tex)
                {
                    m_rhiTexture = std::move(tex);
                }
                else
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "TextureAsset::Load: RHI texture creation failed for '%s'", m_path.c_str());
                }
            }
            // If the bridge isn't up yet (pre-graphics init, tests, etc.)
            // the CPU dimensions above still get reported via GetWidth /
            // GetHeight; a late RHI upload can re-read `m_path` later.

            stbi_image_free(pixels);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "TextureAsset::Load: stbi_load failed for '%s' (%s)",
                           m_path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }
#endif // SPARK_HAS_STB_IMAGE
    }
    m_metadata.memorySize = GetMemoryUsage();
    m_loaded = true;
    return S_OK;
}

void TextureAsset::Unload()
{
    m_width = 0;
    m_height = 0;
    m_rhiTexture.reset();
    m_metadata.state = StreamingState::Unloaded;
    m_metadata.memorySize = 0;
    m_loaded = false;
}

size_t TextureAsset::GetMemoryUsage() const
{
    return static_cast<size_t>(m_width) * m_height * 4;
}

HRESULT AudioAsset::Load(ID3D11Device* /*device*/)
{
    m_metadata.filePath = m_path;
    m_metadata.name = std::filesystem::path(m_path).stem().string();
    m_metadata.type = AssetType::Audio;
    m_metadata.state = StreamingState::Loaded;
    if (std::filesystem::exists(m_path))
    {
        m_metadata.fileSize = std::filesystem::file_size(m_path);
        std::ifstream file(m_path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            auto size = file.tellg();
            if (size > 0)
            {
                m_audioData.resize(static_cast<size_t>(size));
                file.seekg(0, std::ios::beg);
                file.read(reinterpret_cast<char*>(m_audioData.data()), size);
            }
            file.close();
        }
    }
    m_metadata.memorySize = GetMemoryUsage();
    m_loaded = true;
    return S_OK;
}

void AudioAsset::Unload()
{
    m_audioData.clear();
    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_metadata.state = StreamingState::Unloaded;
    m_metadata.memorySize = 0;
    m_loaded = false;
}

size_t AudioAsset::GetMemoryUsage() const
{
    return m_audioData.size();
}

// ============================================================================
// AssetCache (Linux)
// ============================================================================

AssetCache::AssetCache(size_t maxMemoryMB) : m_maxMemory(maxMemoryMB * 1024 * 1024), m_hits(0), m_misses(0) {}

AssetCache::~AssetCache()
{
    Clear();
}

void AssetCache::SetMaxMemory(size_t maxMemoryMB)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxMemory = maxMemoryMB * 1024 * 1024;
}

size_t AssetCache::GetCurrentMemory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto& pair : m_cache)
    {
        if (pair.second.asset)
        {
            total += pair.second.asset->GetMemoryUsage();
        }
    }
    return total;
}

void AssetCache::AddAsset(std::shared_ptr<Asset> asset)
{
    if (!asset)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    CacheEntry entry;
    entry.asset = asset;
    entry.lastAccessed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    entry.accessCount = 1;
    m_cache[asset->GetPath()] = entry;
}

std::shared_ptr<Asset> AssetCache::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        it->second.lastAccessed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        it->second.accessCount++;
        m_hits++;
        return it->second.asset;
    }
    m_misses++;
    return nullptr;
}

void AssetCache::RemoveAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(path);
}

void AssetCache::EvictLRU()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cache.empty())
        return;

    auto oldest = m_cache.begin();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
    {
        if (it->second.lastAccessed < oldest->second.lastAccessed)
        {
            oldest = it;
        }
    }
    if (oldest != m_cache.end())
    {
        if (oldest->second.asset)
        {
            oldest->second.asset->Unload();
        }
        m_cache.erase(oldest);
    }
}

void AssetCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_hits = 0;
    m_misses = 0;
}

float AssetCache::GetHitRatio() const
{
    uint32_t total = m_hits + m_misses;
    return total > 0 ? static_cast<float>(m_hits) / static_cast<float>(total) : 0.0f;
}

#endif // !SPARK_PLATFORM_WINDOWS
