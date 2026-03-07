#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file AssetPipeline.cpp
 * @brief Implementation of complete asset pipeline system
 * @author Spark Engine Team
 * @date 2025
 */

#include "AssetPipeline.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// ASSET BASE CLASS IMPLEMENTATION
// ============================================================================

// Asset base class is header-only, no implementation needed

// ============================================================================
// MESH ASSET IMPLEMENTATION
// ============================================================================

HRESULT MeshAsset::Load(ID3D11Device* device)
{
    ASSERT(device);

    // Attempt to load from file if the path exists
    if (!m_path.empty() && std::filesystem::exists(m_path)) {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".obj") {
            std::ifstream file(m_path);
            if (file.is_open()) {
                std::vector<XMFLOAT3> positions;
                std::vector<XMFLOAT3> normals;
                std::vector<XMFLOAT2> texCoords;
                std::string line;

                XMFLOAT3 bbMin = { FLT_MAX, FLT_MAX, FLT_MAX };
                XMFLOAT3 bbMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

                while (std::getline(file, line)) {
                    std::istringstream iss(line);
                    std::string prefix;
                    iss >> prefix;

                    if (prefix == "v") {
                        XMFLOAT3 pos;
                        iss >> pos.x >> pos.y >> pos.z;
                        positions.push_back(pos);
                        bbMin.x = std::min(bbMin.x, pos.x); bbMin.y = std::min(bbMin.y, pos.y); bbMin.z = std::min(bbMin.z, pos.z);
                        bbMax.x = std::max(bbMax.x, pos.x); bbMax.y = std::max(bbMax.y, pos.y); bbMax.z = std::max(bbMax.z, pos.z);
                    } else if (prefix == "vn") {
                        XMFLOAT3 n;
                        iss >> n.x >> n.y >> n.z;
                        normals.push_back(n);
                    } else if (prefix == "vt") {
                        XMFLOAT2 tc;
                        iss >> tc.x >> tc.y;
                        texCoords.push_back(tc);
                    } else if (prefix == "f") {
                        std::string token;
                        std::vector<uint32_t> faceIndices;
                        while (iss >> token) {
                            MeshAssetData::Vertex vert = {};
                            int vi = 0, vti = 0, vni = 0;
                            if (sscanf(token.c_str(), "%d/%d/%d", &vi, &vti, &vni) >= 1) {
                                if (vi > 0 && vi <= static_cast<int>(positions.size()))
                                    vert.position = positions[vi - 1];
                                if (vti > 0 && vti <= static_cast<int>(texCoords.size()))
                                    vert.texCoord0 = texCoords[vti - 1];
                                if (vni > 0 && vni <= static_cast<int>(normals.size()))
                                    vert.normal = normals[vni - 1];
                            }
                            vert.color = {1.0f, 1.0f, 1.0f, 1.0f};
                            faceIndices.push_back(static_cast<uint32_t>(m_meshData.vertices.size()));
                            m_meshData.vertices.push_back(vert);
                        }
                        for (size_t i = 2; i < faceIndices.size(); ++i) {
                            m_meshData.indices.push_back(faceIndices[0]);
                            m_meshData.indices.push_back(faceIndices[i - 1]);
                            m_meshData.indices.push_back(faceIndices[i]);
                        }
                    }
                }
                m_meshData.boundingBoxMin = bbMin;
                m_meshData.boundingBoxMax = bbMax;
                m_meshData.boundingSphereCenter = {
                    (bbMin.x + bbMax.x) * 0.5f,
                    (bbMin.y + bbMax.y) * 0.5f,
                    (bbMin.z + bbMax.z) * 0.5f
                };
                float dx = bbMax.x - bbMin.x, dy = bbMax.y - bbMin.y, dz = bbMax.z - bbMin.z;
                m_meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
                file.close();

                if (!m_meshData.vertices.empty()) {
                    Spark::SimpleConsole::GetInstance().LogSuccess("Loaded OBJ: " + m_path +
                        " (" + std::to_string(m_meshData.vertices.size()) + " verts, " +
                        std::to_string(m_meshData.indices.size() / 3) + " tris)");
                }
            }
        }
    }

    // Fallback: unit cube if no file was loaded
    if (m_meshData.vertices.empty()) {
        m_meshData.vertices = {
            {{-0.5f,-0.5f,-0.5f},{0,0,-1},{1,0,0},{0,1},{0,0},{1,1,1,1}},
            {{ 0.5f,-0.5f,-0.5f},{0,0,-1},{1,0,0},{1,1},{0,0},{1,1,1,1}},
            {{ 0.5f, 0.5f,-0.5f},{0,0,-1},{1,0,0},{1,0},{0,0},{1,1,1,1}},
            {{-0.5f, 0.5f,-0.5f},{0,0,-1},{1,0,0},{0,0},{0,0},{1,1,1,1}},
            {{-0.5f,-0.5f, 0.5f},{0,0, 1},{-1,0,0},{1,1},{0,0},{1,1,1,1}},
            {{ 0.5f,-0.5f, 0.5f},{0,0, 1},{-1,0,0},{0,1},{0,0},{1,1,1,1}},
            {{ 0.5f, 0.5f, 0.5f},{0,0, 1},{-1,0,0},{0,0},{0,0},{1,1,1,1}},
            {{-0.5f, 0.5f, 0.5f},{0,0, 1},{-1,0,0},{1,0},{0,0},{1,1,1,1}},
        };
        m_meshData.indices = {0,1,2,2,3,0, 4,6,5,6,4,7, 4,0,3,3,7,4, 1,5,6,6,2,1, 3,2,6,6,7,3, 4,1,0,1,4,5};
    }

    // Create GPU buffers
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = static_cast<UINT>(m_meshData.vertices.size() * sizeof(MeshAssetData::Vertex));
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = m_meshData.vertices.data();
    HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.ByteWidth = static_cast<UINT>(m_meshData.indices.size() * sizeof(uint32_t));
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = m_meshData.indices.data();
    hr = device->CreateBuffer(&ibDesc, &ibData, &m_indexBuffer);
    if (FAILED(hr)) return hr;

    m_loaded = true;
    return S_OK;
}

void MeshAsset::Unload()
{
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_meshData.vertices.clear();
    m_meshData.indices.clear();
    m_loaded = false;
}

size_t MeshAsset::GetMemoryUsage() const
{
    return m_meshData.vertices.size() * sizeof(MeshAssetData::Vertex) +
           m_meshData.indices.size() * sizeof(uint32_t);
}

// ============================================================================
// TEXTURE ASSET IMPLEMENTATION
// ============================================================================

HRESULT TextureAsset::Load(ID3D11Device* device)
{
    ASSERT(device);

    std::vector<uint32_t> pixelData;
    bool loadedFromFile = false;

    // Attempt to load from file (TGA format — simple, no external library needed)
    if (!m_path.empty() && std::filesystem::exists(m_path)) {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".tga") {
            std::ifstream file(m_path, std::ios::binary);
            if (file.is_open()) {
                uint8_t header[18];
                file.read(reinterpret_cast<char*>(header), 18);
                if (file.gcount() == 18) {
                    m_width = header[12] | (header[13] << 8);
                    m_height = header[14] | (header[15] << 8);
                    uint8_t bpp = header[16];
                    uint8_t imageType = header[2];

                    if (imageType == 2 && (bpp == 24 || bpp == 32) && m_width > 0 && m_height > 0) {
                        size_t bytesPerPixel = bpp / 8;
                        size_t dataSize = m_width * m_height * bytesPerPixel;
                        std::vector<uint8_t> rawData(dataSize);
                        file.read(reinterpret_cast<char*>(rawData.data()), dataSize);

                        pixelData.resize(m_width * m_height);
                        for (uint32_t i = 0; i < m_width * m_height; ++i) {
                            uint8_t b = rawData[i * bytesPerPixel + 0];
                            uint8_t g = rawData[i * bytesPerPixel + 1];
                            uint8_t r = rawData[i * bytesPerPixel + 2];
                            uint8_t a = (bpp == 32) ? rawData[i * bytesPerPixel + 3] : 255;
                            pixelData[i] = (a << 24) | (b << 16) | (g << 8) | r;
                        }
                        loadedFromFile = true;
                        Spark::SimpleConsole::GetInstance().LogSuccess("Loaded TGA: " + m_path +
                            " (" + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
                    }
                }
            }
        } else if (ext == ".dds") {
            // DDS can be loaded directly via D3D11 CreateDDSTextureFromFile
            // For now, log that it needs the DirectXTex helper
            Spark::SimpleConsole::GetInstance().LogWarning("DDS loading requires DirectXTex — using fallback for: " + m_path);
        }
    }

    // Fallback: 2x2 checkerboard
    if (!loadedFromFile) {
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
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_srv);
    if (FAILED(hr)) return hr;

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
    return m_width * m_height * 4; // Assuming 4 bytes per pixel
}

// ============================================================================
// AUDIO ASSET IMPLEMENTATION
// ============================================================================

HRESULT AudioAsset::Load(ID3D11Device* device)
{
    bool loadedFromFile = false;

    // Attempt to load WAV file
    if (!m_path.empty() && std::filesystem::exists(m_path)) {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".wav") {
            std::ifstream file(m_path, std::ios::binary);
            if (file.is_open()) {
                // Read RIFF header
                char riff[4], wave[4], fmt[4];
                uint32_t fileSize, fmtSize, dataSize;
                uint16_t audioFormat, numChannels, bitsPerSample;
                uint32_t sampleRate, byteRate;
                uint16_t blockAlign;

                file.read(riff, 4);
                file.read(reinterpret_cast<char*>(&fileSize), 4);
                file.read(wave, 4);

                if (std::string(riff, 4) == "RIFF" && std::string(wave, 4) == "WAVE") {
                    // Find fmt chunk
                    bool foundFmt = false, foundData = false;
                    while (file.good() && !(foundFmt && foundData)) {
                        char chunkId[4];
                        uint32_t chunkSize;
                        file.read(chunkId, 4);
                        file.read(reinterpret_cast<char*>(&chunkSize), 4);
                        std::string id(chunkId, 4);

                        if (id == "fmt ") {
                            file.read(reinterpret_cast<char*>(&audioFormat), 2);
                            file.read(reinterpret_cast<char*>(&numChannels), 2);
                            file.read(reinterpret_cast<char*>(&sampleRate), 4);
                            file.read(reinterpret_cast<char*>(&byteRate), 4);
                            file.read(reinterpret_cast<char*>(&blockAlign), 2);
                            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
                            // Skip extra fmt bytes
                            if (chunkSize > 16)
                                file.seekg(chunkSize - 16, std::ios::cur);
                            foundFmt = true;
                        } else if (id == "data") {
                            dataSize = chunkSize;
                            m_audioData.resize(dataSize);
                            file.read(reinterpret_cast<char*>(m_audioData.data()), dataSize);
                            foundData = true;
                        } else {
                            file.seekg(chunkSize, std::ios::cur);
                        }
                    }

                    if (foundFmt && foundData && audioFormat == 1) { // PCM only
                        m_sampleRate = sampleRate;
                        m_channels = numChannels;
                        m_bitsPerSample = bitsPerSample;
                        loadedFromFile = true;
                        Spark::SimpleConsole::GetInstance().LogSuccess("Loaded WAV: " + m_path +
                            " (" + std::to_string(m_sampleRate) + " Hz, " +
                            std::to_string(m_channels) + " ch, " +
                            std::to_string(m_bitsPerSample) + " bit)");
                    }
                }
            }
        }
    }

    // Fallback: 1 second of silence
    if (!loadedFromFile) {
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
// ASSET CACHE IMPLEMENTATION
// ============================================================================

AssetCache::AssetCache(size_t maxMemoryMB)
    : m_maxMemory(maxMemoryMB * 1024 * 1024)
{
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
    
    for (const auto& pair : m_cache) {
        totalMemory += pair.second.asset->GetMemoryUsage();
    }
    
    return totalMemory;
}

void AssetCache::AddAsset(std::shared_ptr<Asset> asset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    CacheEntry entry;
    entry.asset = asset;
    entry.lastAccessed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    entry.accessCount = 1;
    
    m_cache[asset->GetPath()] = entry;
    
    // Evict if over budget
    while (GetCurrentMemory() > m_maxMemory) {
        EvictLRU();
    }
}

std::shared_ptr<Asset> AssetCache::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        // Update access info
        it->second.lastAccessed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
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
    if (m_cache.empty()) return;
    
    // Find least recently used asset
    auto oldestIt = m_cache.begin();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        if (it->second.lastAccessed < oldestIt->second.lastAccessed) {
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

// ============================================================================
// ASSET PIPELINE IMPLEMENTATION
// ============================================================================

AssetPipeline::AssetPipeline()
    : m_device(nullptr), m_context(nullptr)
{
}

AssetPipeline::~AssetPipeline()
{
    Shutdown();
}

HRESULT AssetPipeline::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT(device && context);
    
    m_device = device;
    m_context = context;
    
    // Initialize cache
    m_cache = std::make_unique<AssetCache>(512); // 512MB default
    
    // Initialize metrics
    memset(&m_metrics, 0, sizeof(m_metrics));
    
    // Start loading threads
    SetStreamingThreadCount(2);
    
    Spark::SimpleConsole::GetInstance().LogSuccess("AssetPipeline initialized successfully");
    return S_OK;
}

void AssetPipeline::Shutdown()
{
    // Stop loading threads
    m_shouldStop = true;
    m_queueCondition.notify_all();
    
    for (auto& thread : m_loadingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Clear assets
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.clear();
    }
    
    m_cache.reset();
    m_device = nullptr;
    m_context = nullptr;
    
    Spark::SimpleConsole::GetInstance().LogInfo("AssetPipeline shutdown complete");
}

void AssetPipeline::Update(float deltaTime)
{
    UpdateMetrics();
    
    if (m_hotReloadingEnabled) {
        CheckForChangedAssets();
    }
}

std::shared_ptr<Asset> AssetPipeline::LoadAsset(const std::string& path, AssetType type)
{
    // Check cache first
    auto cachedAsset = m_cache->GetAsset(path);
    if (cachedAsset) {
        return cachedAsset;
    }
    
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        if (it != m_assets.end()) {
            return it->second;
        }
    }
    
    // Detect type if unknown
    if (type == AssetType::Unknown) {
        type = DetectAssetType(path);
    }
    
    // Load based on type
    std::shared_ptr<Asset> asset;
    switch (type) {
        case AssetType::Mesh:
            asset = LoadMeshFromFile(path);
            break;
        case AssetType::Texture:
            asset = LoadTextureFromFile(path);
            break;
        case AssetType::Audio:
            asset = LoadAudioFromFile(path);
            break;
        default:
            Spark::SimpleConsole::GetInstance().LogError("Unsupported asset type for: " + path);
            return nullptr;
    }
    
    if (asset) {
        // Add to cache and assets
        m_cache->AddAsset(asset);
        
        {
            std::lock_guard<std::mutex> lock(m_assetsMutex);
            m_assets[path] = asset;
        }
        
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedAssets++;
    }
    
    return asset;
}

std::shared_ptr<MeshAsset> AssetPipeline::LoadMesh(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Mesh);
    return std::dynamic_pointer_cast<MeshAsset>(asset);
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTexture(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Texture);
    return std::dynamic_pointer_cast<TextureAsset>(asset);
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudio(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Audio);
    return std::dynamic_pointer_cast<AudioAsset>(asset);
}

void AssetPipeline::LoadAssetAsync(const AssetLoadRequest& request)
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_loadQueue.push(request);
    }
    
    m_queueCondition.notify_one();
}

void AssetPipeline::LoadMeshAsync(const std::string& path, std::function<void(std::shared_ptr<MeshAsset>)> callback)
{
    AssetLoadRequest request;
    request.assetPath = path;
    request.expectedType = AssetType::Mesh;
    request.priority = LoadingPriority::Normal;
    request.onLoaded = [callback](std::shared_ptr<void> asset) {
        if (callback) {
            callback(std::static_pointer_cast<MeshAsset>(asset));
        }
    };
    
    LoadAssetAsync(request);
}

void AssetPipeline::LoadTextureAsync(const std::string& path, std::function<void(std::shared_ptr<TextureAsset>)> callback)
{
    AssetLoadRequest request;
    request.assetPath = path;
    request.expectedType = AssetType::Texture;
    request.priority = LoadingPriority::Normal;
    request.onLoaded = [callback](std::shared_ptr<void> asset) {
        if (callback) {
            callback(std::static_pointer_cast<TextureAsset>(asset));
        }
    };
    
    LoadAssetAsync(request);
}

void AssetPipeline::UnloadAsset(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.erase(path);
    }
    
    m_cache->RemoveAsset(path);
    
    std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
    m_metrics.loadedAssets--;
}

void AssetPipeline::UnloadAllAssets()
{
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.clear();
    }
    
    m_cache->Clear();
    
    std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
    m_metrics.loadedAssets = 0;
}

std::shared_ptr<Asset> AssetPipeline::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    return (it != m_assets.end()) ? it->second : nullptr;
}

bool AssetPipeline::IsAssetLoaded(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    return m_assets.find(path) != m_assets.end();
}

void AssetPipeline::SetCacheSize(size_t maxMemoryMB)
{
    if (m_cache) {
        m_cache->SetMaxMemory(maxMemoryMB);
    }
}

void AssetPipeline::EvictUnusedAssets()
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    
    auto it = m_assets.begin();
    while (it != m_assets.end()) {
        if (it->second.use_count() == 1) {
            Spark::SimpleConsole::GetInstance().LogInfo("Evicting unused asset: " + it->first);
            it = m_assets.erase(it);
        } else {
            ++it;
        }
    }
}

void AssetPipeline::PreloadAssets(const std::vector<std::string>& paths)
{
    for (const auto& path : paths) {
        LoadAsset(path);
    }
}

void AssetPipeline::EnableBackgroundStreaming(bool enabled)
{
    m_backgroundStreaming = enabled;
}

void AssetPipeline::SetStreamingThreadCount(int count)
{
    // Stop existing threads
    m_shouldStop = true;
    m_queueCondition.notify_all();
    
    for (auto& thread : m_loadingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    m_loadingThreads.clear();
    m_shouldStop = false;
    
    // Start new threads
    for (int i = 0; i < count; ++i) {
        m_loadingThreads.emplace_back(&AssetPipeline::LoadingThreadFunction, this);
    }
}

std::vector<std::string> AssetPipeline::ScanDirectory(const std::string& directory, AssetType type)
{
    std::vector<std::string> assets;
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                AssetType detectedType = DetectAssetType(entry.path().string());
                if (type == AssetType::Unknown || detectedType == type) {
                    assets.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::exception& e) {
        Spark::SimpleConsole::GetInstance().LogError("Error scanning directory: " + directory + " - " + e.what());
    }
    
    return assets;
}

AssetType AssetPipeline::DetectAssetType(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return DetectAssetTypeFromExtension(ext);
}

AssetMetadata AssetPipeline::GetAssetMetadata(const std::string& path)
{
    AssetMetadata metadata;
    metadata.filePath = path;
    metadata.name = std::filesystem::path(path).stem().string();
    metadata.type = DetectAssetType(path);
    
    if (std::filesystem::exists(path)) {
        metadata.fileSize = std::filesystem::file_size(path);
        metadata.lastModified = GetFileTimestamp(path);
        metadata.checksum = CalculateChecksum(path);
    }
    
    return metadata;
}

void AssetPipeline::RefreshAssetMetadata(const std::string& path)
{
    // Implementation would refresh metadata from file
}

void AssetPipeline::CheckForChangedAssets()
{
    // Implementation would check file timestamps and reload changed assets
}

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// Console integration methods
AssetPipeline::AssetMetrics AssetPipeline::Console_GetMetrics() const
{
    return GetMetrics();
}

std::string AssetPipeline::Console_ListAssets() const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    std::stringstream ss;
    
    ss << "=== Loaded Assets (" << m_assets.size() << ") ===\n";
    for (const auto& pair : m_assets) {
        const auto& asset = pair.second;
        ss << pair.first << " - " << AssetTypeToString(asset->GetType());
        ss << " (" << (asset->GetMemoryUsage() / 1024) << " KB)\n";
    }
    
    return ss.str();
}

std::string AssetPipeline::Console_GetAssetInfo(const std::string& path) const
{
    std::shared_ptr<Asset> asset;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        asset = (it != m_assets.end()) ? it->second : nullptr;
    }
    
    if (!asset) {
        return "Asset not found: " + path;
    }
    
    std::stringstream ss;
    ss << "=== Asset Info: " << path << " ===\n";
    ss << "Type: " << AssetTypeToString(asset->GetType()) << "\n";
    ss << "Memory Usage: " << (asset->GetMemoryUsage() / 1024) << " KB\n";
    ss << "Loaded: " << (asset->IsLoaded() ? "Yes" : "No") << "\n";
    
    return ss.str();
}

bool AssetPipeline::Console_LoadAsset(const std::string& path)
{
    auto asset = LoadAsset(path);
    return asset != nullptr;
}

bool AssetPipeline::Console_UnloadAsset(const std::string& path)
{
    UnloadAsset(path);
    return true;
}

void AssetPipeline::Console_SetCacheSize(size_t maxMemoryMB)
{
    SetCacheSize(maxMemoryMB);
    Spark::SimpleConsole::GetInstance().LogSuccess("Asset cache size set to: " + std::to_string(maxMemoryMB) + " MB");
}

void AssetPipeline::Console_ForceGC()
{
    EvictUnusedAssets();
    Spark::SimpleConsole::GetInstance().LogSuccess("Asset garbage collection completed");
}

void AssetPipeline::Console_EnableStreaming(bool enabled)
{
    EnableBackgroundStreaming(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Background streaming " + std::string(enabled ? "enabled" : "disabled"));
}

void AssetPipeline::Console_SetStreamingThreads(int count)
{
    SetStreamingThreadCount(count);
    Spark::SimpleConsole::GetInstance().LogSuccess("Streaming thread count set to: " + std::to_string(count));
}

int AssetPipeline::Console_ScanDirectory(const std::string& directory)
{
    auto assets = ScanDirectory(directory);
    Spark::SimpleConsole::GetInstance().LogSuccess("Found " + std::to_string(assets.size()) + " assets in: " + directory);
    return static_cast<int>(assets.size());
}

void AssetPipeline::Console_EnableHotReload(bool enabled)
{
    EnableHotReloading(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Hot reloading " + std::string(enabled ? "enabled" : "disabled"));
}

int AssetPipeline::Console_PreloadDirectory(const std::string& directory)
{
    auto assets = ScanDirectory(directory);
    PreloadAssets(assets);
    Spark::SimpleConsole::GetInstance().LogSuccess("Preloaded " + std::to_string(assets.size()) + " assets from: " + directory);
    return static_cast<int>(assets.size());
}

int AssetPipeline::Console_ReloadAllAssets()
{
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        count = static_cast<int>(m_assets.size());
    }
    
    UnloadAllAssets();
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked " + std::to_string(count) + " assets for reload");
    return count;
}

// Private helper methods
void AssetPipeline::LoadingThreadFunction()
{
    while (!m_shouldStop) {
        AssetLoadRequest request;
        bool hasRequest = false;
        
        // Get next request
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] { return !m_loadQueue.empty() || m_shouldStop; });
            
            if (m_shouldStop) break;
            
            if (!m_loadQueue.empty()) {
                request = m_loadQueue.front();
                m_loadQueue.pop();
                hasRequest = true;
            }
        }
        
        if (hasRequest) {
            // Load asset
            auto asset = LoadAsset(request.assetPath, request.expectedType);
            
            if (asset && request.onLoaded) {
                request.onLoaded(asset);
            } else if (!asset && request.onError) {
                request.onError("Failed to load asset: " + request.assetPath);
            }
        }
    }
}

AssetType AssetPipeline::DetectAssetTypeFromExtension(const std::string& extension)
{
    if (extension == ".obj" || extension == ".fbx" || extension == ".dae" || extension == ".gltf" || extension == ".glb") {
        return AssetType::Mesh;
    } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".dds") {
        return AssetType::Texture;
    } else if (extension == ".wav" || extension == ".mp3" || extension == ".ogg") {
        return AssetType::Audio;
    } else if (extension == ".hlsl" || extension == ".fx") {
        return AssetType::Shader;
    } else if (extension == ".ttf" || extension == ".otf") {
        return AssetType::Font;
    }
    
    return AssetType::Unknown;
}

std::string AssetPipeline::CalculateChecksum(const std::string& filePath)
{
    // Simple checksum implementation - in production would use MD5/SHA
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return "";
    
    size_t hash = 0;
    char buffer[1024];
    while (file.read(buffer, sizeof(buffer))) {
        for (size_t i = 0; i < file.gcount(); ++i) {
            hash = hash * 31 + buffer[i];
        }
    }
    
    return std::to_string(hash);
}

uint64_t AssetPipeline::GetFileTimestamp(const std::string& filePath)
{
    try {
        auto time = std::filesystem::last_write_time(filePath);
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    } catch (...) {
        return 0;
    }
}

void AssetPipeline::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    
    {
        std::lock_guard<std::mutex> assetsLock(m_assetsMutex);
        m_metrics.totalAssets = static_cast<uint32_t>(m_assets.size());
    }
    
    // Update other metrics
    m_metrics.streamingThreads = static_cast<uint32_t>(m_loadingThreads.size());
    m_metrics.backgroundLoading = m_backgroundStreaming;
    
    if (m_cache) {
        m_metrics.cacheHitRatio = m_cache->GetHitRatio();
        m_metrics.memoryUsage = m_cache->GetCurrentMemory();
    }
}

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    auto meshAsset = std::make_shared<MeshAsset>(path);
    HRESULT hr = meshAsset->Load(m_device);
    
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load mesh: " + path);
        return nullptr;
    }
    
    return meshAsset;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto textureAsset = std::make_shared<TextureAsset>(path);
    HRESULT hr = textureAsset->Load(m_device);
    
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + path);
        return nullptr;
    }
    
    return textureAsset;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audioAsset = std::make_shared<AudioAsset>(path);
    HRESULT hr = audioAsset->Load(m_device);
    
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load audio: " + path);
        return nullptr;
    }
    
    return audioAsset;
}

// Utility functions
std::string AssetTypeToString(AssetType type)
{
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::Audio: return "Audio";
        case AssetType::Animation: return "Animation";
        case AssetType::Prefab: return "Prefab";
        case AssetType::Scene: return "Scene";
        case AssetType::Shader: return "Shader";
        case AssetType::Font: return "Font";
        default: return "Unknown";
    }
}

AssetType StringToAssetType(const std::string& str)
{
    if (str == "Mesh") return AssetType::Mesh;
    if (str == "Texture") return AssetType::Texture;
    if (str == "Material") return AssetType::Material;
    if (str == "Audio") return AssetType::Audio;
    if (str == "Animation") return AssetType::Animation;
    if (str == "Prefab") return AssetType::Prefab;
    if (str == "Scene") return AssetType::Scene;
    if (str == "Shader") return AssetType::Shader;
    if (str == "Font") return AssetType::Font;
    return AssetType::Unknown;
}

std::string StreamingStateToString(StreamingState state)
{
    switch (state) {
        case StreamingState::Unloaded: return "Unloaded";
        case StreamingState::Loading: return "Loading";
        case StreamingState::Loaded: return "Loaded";
        case StreamingState::Failed: return "Failed";
        case StreamingState::Evicted: return "Evicted";
        default: return "Unknown";
    }
}

std::string LoadingPriorityToString(LoadingPriority priority)
{
    switch (priority) {
        case LoadingPriority::Low: return "Low";
        case LoadingPriority::Normal: return "Normal";
        case LoadingPriority::High: return "High";
        case LoadingPriority::Critical: return "Critical";
        default: return "Unknown";
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

// Linux no-op stubs
#include "AssetPipeline.h"
AssetPipeline::AssetPipeline() {}
AssetPipeline::~AssetPipeline() {}
AssetCache::AssetCache(size_t) {}
AssetCache::~AssetCache() {}

#endif // SPARK_PLATFORM_WINDOWS
