/**
 * @file TextureSystem.h
 * @brief Advanced texture loading and management system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides comprehensive texture management including loading,
 * streaming, compression, mip-map generation, and memory optimization.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

/**
 * @brief Texture formats supported by the system
 */
enum class TextureFormat
{
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    BC1_UNORM,      // DXT1
    BC1_SRGB,
    BC3_UNORM,      // DXT5
    BC3_SRGB,
    BC7_UNORM,
    BC7_SRGB,
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    D24_UNORM_S8_UINT,
    R16_FLOAT,
    R32_FLOAT
};

/**
 * @brief Texture types
 */
enum class TextureType
{
    Texture2D,
    TextureCube,
    Texture3D,
    TextureArray
};

/**
 * @brief Texture usage flags
 */
enum class TextureUsage : uint32_t
{
    None = 0,
    ShaderResource = 1 << 0,
    RenderTarget = 1 << 1,
    DepthStencil = 1 << 2,
    UnorderedAccess = 1 << 3,
    Dynamic = 1 << 4,
    Staging = 1 << 5
};

/**
 * @brief Texture quality settings
 */
enum class TextureQuality
{
    Low,        // Quarter resolution, high compression
    Medium,     // Half resolution, medium compression
    High,       // Full resolution, low compression
    Ultra       // Full resolution, no compression
};

/**
 * @brief Texture description
 */
struct TextureDesc
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    TextureFormat format = TextureFormat::R8G8B8A8_UNORM;
    TextureType type = TextureType::Texture2D;
    TextureUsage usage = TextureUsage::ShaderResource;
    bool generateMips = true;
    bool sRGB = false;
    uint32_t sampleCount = 1;
    uint32_t sampleQuality = 0;
};

/**
 * @brief Texture resource
 */
class Texture
{
public:
    Texture(const std::string& name, const TextureDesc& desc);
    ~Texture() = default;

    // Getters
    const std::string& GetName() const { return m_name; }
    const TextureDesc& GetDesc() const { return m_desc; }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
    ID3D11RenderTargetView* GetRTV() const { return m_rtv.Get(); }
    ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }
    ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
    ID3D11Resource* GetResource() const { return m_resource.Get(); }
    
    // Status
    bool IsLoaded() const { return m_loaded; }
    bool IsStreaming() const { return m_streaming; }
    size_t GetMemoryUsage() const { return m_memoryUsage; }
    
    // Resource creation
    HRESULT CreateFromFile(const std::string& filePath, ID3D11Device* device);
    HRESULT CreateFromData(const void* data, size_t dataSize, ID3D11Device* device);
    HRESULT CreateRenderTarget(ID3D11Device* device);
    HRESULT CreateDepthStencil(ID3D11Device* device);
    
    // Resource management
    void Release();
    void Bind(ID3D11DeviceContext* context, uint32_t slot);
    void UnBind(ID3D11DeviceContext* context, uint32_t slot);

private:
    std::string m_name;
    TextureDesc m_desc;
    ComPtr<ID3D11Resource> m_resource;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11UnorderedAccessView> m_uav;
    bool m_loaded = false;
    bool m_streaming = false;
    size_t m_memoryUsage = 0;
    
    HRESULT CreateViews(ID3D11Device* device);
    DXGI_FORMAT GetDXGIFormat(TextureFormat format) const;
};

/**
 * @brief Texture streaming request
 */
struct StreamingRequest
{
    std::string filePath;
    TextureDesc desc;
    std::function<void(std::shared_ptr<Texture>)> callback;
    int priority = 0;
    bool urgent = false;
};

/**
 * @brief Texture system manager
 */
class TextureSystem
{
public:
    /**
     * @brief Texture system metrics
     */
    struct TextureMetrics
    {
        uint32_t loadedTextures;
        uint32_t streamingTextures;
        size_t totalMemoryUsage;
        size_t systemMemoryUsage;
        size_t videoMemoryUsage;
        uint32_t textureBinds;
        uint32_t textureSwitches;
        float averageLoadTime;
        uint32_t compressionRatio;
        uint32_t mipLevelsGenerated;
    };

    TextureSystem();
    ~TextureSystem();

    /**
     * @brief Initialize the texture system
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the texture system
     */
    void Shutdown();

    /**
     * @brief Update the texture system (handle streaming, etc.)
     */
    void Update(float deltaTime);

    // Synchronous loading
    std::shared_ptr<Texture> LoadTexture(const std::string& filePath, const TextureDesc& desc = {});
    std::shared_ptr<Texture> CreateTexture(const std::string& name, const TextureDesc& desc);
    
    // Asynchronous loading/streaming
    void LoadTextureAsync(const std::string& filePath, 
                         std::function<void(std::shared_ptr<Texture>)> callback,
                         const TextureDesc& desc = {});
    
    // Texture management
    std::shared_ptr<Texture> GetTexture(const std::string& name) const;
    void UnloadTexture(const std::string& name);
    void UnloadAllTextures();
    
    // Default textures
    std::shared_ptr<Texture> GetWhiteTexture() const { return m_whiteTexture; }
    std::shared_ptr<Texture> GetBlackTexture() const { return m_blackTexture; }
    std::shared_ptr<Texture> GetNormalTexture() const { return m_normalTexture; }
    std::shared_ptr<Texture> GetNoiseTexture() const { return m_noiseTexture; }
    
    // Quality settings
    void SetTextureQuality(TextureQuality quality) { m_quality = quality; }
    TextureQuality GetTextureQuality() const { return m_quality; }
    
    // Memory management
    void SetMemoryBudget(size_t budgetBytes) { m_memoryBudget = budgetBytes; }
    size_t GetMemoryBudget() const { return m_memoryBudget; }
    size_t GetMemoryUsage() const;
    void GarbageCollect();
    
    // Streaming
    void EnableStreaming(bool enabled) { m_streamingEnabled = enabled; }
    bool IsStreamingEnabled() const { return m_streamingEnabled; }
    void SetStreamingThreadCount(int count);
    
    // Console integration
    TextureMetrics Console_GetMetrics() const;
    std::string Console_ListTextures() const;
    std::string Console_GetTextureInfo(const std::string& name) const;
    void Console_SetQuality(const std::string& quality);
    void Console_SetMemoryBudget(size_t budgetMB);
    void Console_ForceGC();
    void Console_ReloadTexture(const std::string& name);
    void Console_ReloadAllTextures();

private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
    
    // Texture storage
    mutable std::mutex m_texturesMutex;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
    
    // Default textures
    std::shared_ptr<Texture> m_whiteTexture;
    std::shared_ptr<Texture> m_blackTexture;
    std::shared_ptr<Texture> m_normalTexture;
    std::shared_ptr<Texture> m_noiseTexture;
    
    // Settings
    TextureQuality m_quality = TextureQuality::High;
    size_t m_memoryBudget = 512 * 1024 * 1024; // 512MB default
    
    // Streaming
    bool m_streamingEnabled = true;
    std::vector<std::thread> m_streamingThreads;
    std::queue<StreamingRequest> m_streamingQueue;
    mutable std::mutex m_streamingMutex;
    std::condition_variable m_streamingCondition;
    std::atomic<bool> m_shouldStop{false};
    
    // Metrics
    mutable std::mutex m_metricsMutex;
    TextureMetrics m_metrics;
    
    // Helper methods
    HRESULT CreateDefaultTextures();
    void StreamingThreadFunction();
    void UpdateMetrics();
    TextureDesc AdjustDescForQuality(const TextureDesc& desc) const;
    std::shared_ptr<Texture> LoadTextureFromFile(const std::string& filePath, const TextureDesc& desc);
    bool IsTextureFormatSupported(TextureFormat format) const;
    uint32_t CalculateMemoryUsage(const TextureDesc& desc) const;
};

// Utility functions
TextureFormat GetOptimalFormat(const std::string& filePath, bool sRGB = false);
bool IsCompressedFormat(TextureFormat format);
uint32_t GetFormatBlockSize(TextureFormat format);
uint32_t GetFormatBytesPerPixel(TextureFormat format);
