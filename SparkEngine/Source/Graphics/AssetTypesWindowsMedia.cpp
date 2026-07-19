/**
 * @file AssetTypesWindowsMedia.cpp
 * @brief Windows/D3D11 texture/audio asset implementations (TextureAsset, AudioAsset) and AssetCache
 *
 * Contains the D3D11 texture creation, TGA/WAV loading, and Windows-specific
 * asset cache logic. Split from AssetTypesWindows.cpp, which keeps the
 * MeshAsset loaders. The Linux counterpart lives in AssetTypesLinuxMedia.cpp.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

// ============================================================================
// TEXTURE ASSET IMPLEMENTATION (Windows / D3D11)
// ============================================================================

HRESULT TextureAsset::Load(ID3D11Device* device)
{
    ASSERT(device);

    std::vector<uint32_t> pixelData;
    bool loadedFromFile = false;

    // Attempt to load from file (TGA format — simple, no external library needed)
    if (!m_path.empty() && std::filesystem::exists(m_path))
    {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".tga")
        {
            std::ifstream file(m_path, std::ios::binary);
            if (file.is_open())
            {
                uint8_t header[18];
                file.read(reinterpret_cast<char*>(header), 18);
                if (file.gcount() == 18)
                {
                    m_width = header[12] | (header[13] << 8);
                    m_height = header[14] | (header[15] << 8);
                    uint8_t bpp = header[16];
                    uint8_t imageType = header[2];

                    if (imageType == 2 && (bpp == 24 || bpp == 32) && m_width > 0 && m_height > 0 && m_width <= 65536 &&
                        m_height <= 65536)
                    {
                        size_t bytesPerPixel = bpp / 8;
                        size_t dataSize = static_cast<size_t>(m_width) * m_height * bytesPerPixel;
                        std::vector<uint8_t> rawData(dataSize);
                        file.read(reinterpret_cast<char*>(rawData.data()), dataSize);

                        size_t pixelCount = static_cast<size_t>(m_width) * m_height;
                        pixelData.resize(pixelCount);
                        for (size_t i = 0; i < pixelCount; ++i)
                        {
                            uint8_t b = rawData[i * bytesPerPixel + 0];
                            uint8_t g = rawData[i * bytesPerPixel + 1];
                            uint8_t r = rawData[i * bytesPerPixel + 2];
                            uint8_t a = (bpp == 32) ? rawData[i * bytesPerPixel + 3] : 255;
                            pixelData[i] = (a << 24) | (b << 16) | (g << 8) | r;
                        }
                        loadedFromFile = true;
                        Spark::SimpleConsole::GetInstance().LogSuccess("Loaded TGA: " + m_path + " (" +
                                                                       std::to_string(m_width) + "x" +
                                                                       std::to_string(m_height) + ")");
                    }
                }
            }
        }
        else if (ext == ".dds")
        {
            Spark::SimpleConsole::GetInstance().LogWarning("DDS loading requires DirectXTex — using fallback for: " +
                                                           m_path);
        }
    }

    // Fallback: 2x2 checkerboard
    if (!loadedFromFile)
    {
        m_width = 2;
        m_height = 2;
        pixelData = {0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF};
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixelData.data();
    initData.SysMemPitch = m_width * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &m_texture);
    if (FAILED(hr))
        return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_srv);
    if (FAILED(hr))
        return hr;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TextureAsset loaded: %ux%u", m_width, m_height);
    m_loaded = true;
    return S_OK;
}

void TextureAsset::Unload()
{
    m_srv.Reset();
    m_texture.Reset();
    m_width = 0;
    m_height = 0;
    m_loaded = false;
}

size_t TextureAsset::GetMemoryUsage() const
{
    return static_cast<size_t>(m_width) * m_height * 4;
}

// ============================================================================
// AUDIO ASSET IMPLEMENTATION (Windows)
// ============================================================================

HRESULT AudioAsset::Load(ID3D11Device* device)
{
    bool loadedFromFile = false;

    if (!m_path.empty() && std::filesystem::exists(m_path))
    {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".wav")
        {
            std::ifstream file(m_path, std::ios::binary);
            if (file.is_open())
            {
                char riff[4], wave[4];
                uint32_t fileSize, dataSize;
                uint16_t audioFormat, numChannels, bitsPerSample;
                uint32_t sampleRate, byteRate;
                uint16_t blockAlign;

                file.read(riff, 4);
                file.read(reinterpret_cast<char*>(&fileSize), 4);
                file.read(wave, 4);

                if (std::string(riff, 4) == "RIFF" && std::string(wave, 4) == "WAVE")
                {
                    bool foundFmt = false, foundData = false;
                    while (file.good() && !(foundFmt && foundData))
                    {
                        char chunkId[4];
                        uint32_t chunkSize;
                        file.read(chunkId, 4);
                        file.read(reinterpret_cast<char*>(&chunkSize), 4);
                        std::string id(chunkId, 4);

                        if (id == "fmt ")
                        {
                            file.read(reinterpret_cast<char*>(&audioFormat), 2);
                            file.read(reinterpret_cast<char*>(&numChannels), 2);
                            file.read(reinterpret_cast<char*>(&sampleRate), 4);
                            file.read(reinterpret_cast<char*>(&byteRate), 4);
                            file.read(reinterpret_cast<char*>(&blockAlign), 2);
                            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
                            if (chunkSize > 16)
                                file.seekg(chunkSize - 16, std::ios::cur);
                            foundFmt = true;
                        }
                        else if (id == "data")
                        {
                            dataSize = chunkSize;
                            m_audioData.resize(dataSize);
                            file.read(reinterpret_cast<char*>(m_audioData.data()), dataSize);
                            foundData = true;
                        }
                        else
                        {
                            file.seekg(chunkSize, std::ios::cur);
                        }
                    }

                    if (foundFmt && foundData && audioFormat == 1)
                    {
                        m_sampleRate = sampleRate;
                        m_channels = numChannels;
                        m_bitsPerSample = bitsPerSample;
                        loadedFromFile = true;
                        Spark::SimpleConsole::GetInstance().LogSuccess(
                            "Loaded WAV: " + m_path + " (" + std::to_string(m_sampleRate) + " Hz, " +
                            std::to_string(m_channels) + " ch, " + std::to_string(m_bitsPerSample) + " bit)");
                    }
                }
            }
        }
    }

    // Fallback: 1 second of silence
    if (!loadedFromFile)
    {
        m_sampleRate = 44100;
        m_channels = 2;
        m_bitsPerSample = 16;
        size_t dataSize = m_sampleRate * m_channels * (m_bitsPerSample / 8);
        m_audioData.resize(dataSize, 0);
    }

    m_loaded = true;
    return S_OK;
}

void AudioAsset::Unload()
{
    m_audioData.clear();
    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_loaded = false;
}

size_t AudioAsset::GetMemoryUsage() const
{
    return m_audioData.size();
}

// ============================================================================
// ASSET CACHE IMPLEMENTATION (Windows)
// ============================================================================

AssetCache::AssetCache(size_t maxMemoryMB) : m_maxMemory(maxMemoryMB * 1024 * 1024)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AssetCache created with %zu MB budget", maxMemoryMB);
}

AssetCache::~AssetCache()
{
    Clear();
}

void AssetCache::SetMaxMemory(size_t maxMemoryMB)
{
    m_maxMemory = maxMemoryMB * 1024 * 1024;
}

size_t AssetCache::GetCurrentMemory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t totalMemory = 0;

    for (const auto& pair : m_cache)
    {
        totalMemory += pair.second.asset->GetMemoryUsage();
    }

    return totalMemory;
}

void AssetCache::AddAsset(std::shared_ptr<Asset> asset)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    CacheEntry entry;
    entry.asset = asset;
    entry.lastAccessed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    entry.accessCount = 1;

    m_cache[asset->GetPath()] = entry;

    // Evict if over budget
    while (GetCurrentMemory() > m_maxMemory)
    {
        EvictLRU();
    }
}

std::shared_ptr<Asset> AssetCache::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        it->second.lastAccessed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        it->second.accessCount++;

        m_hits++;
        return it->second.asset;
    }

    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "AssetCache miss: '%s' (hits=%u, misses=%u)", path.c_str(), m_hits,
                    m_misses);
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
    if (m_cache.empty())
        return;

    auto oldestIt = m_cache.begin();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
    {
        if (it->second.lastAccessed < oldestIt->second.lastAccessed)
        {
            oldestIt = it;
        }
    }

    m_cache.erase(oldestIt);
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
    return (total > 0) ? static_cast<float>(m_hits) / total : 0.0f;
}

#endif // SPARK_PLATFORM_WINDOWS
