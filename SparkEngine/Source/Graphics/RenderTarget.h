/**
 * @file RenderTarget.h
 * @brief Advanced render target system for deferred rendering and post-processing
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive render target system supporting multiple
 * render targets (MRT), different pixel formats, MSAA, and console integration
 * for advanced rendering techniques like deferred rendering and post-processing.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <utility>

using Microsoft::WRL::ComPtr;

/**
 * @brief Render target formats for different rendering passes
 */
enum class RenderTargetFormat
{
    // Standard formats
    RGBA8_UNORM,  ///< 8-bit RGBA (LDR albedo, UI)
    RGBA8_SRGB,   ///< 8-bit sRGB RGBA (gamma-corrected albedo)
    RGBA16_FLOAT, ///< 16-bit float RGBA (HDR color, normals)
    RGBA32_FLOAT, ///< 32-bit float RGBA (high precision)

    // Specialized formats
    RG16_FLOAT, ///< 16-bit float RG (velocity, depth derivatives)
    RG32_FLOAT, ///< 32-bit float RG (motion vectors)
    R32_FLOAT,  ///< 32-bit float R (depth, shadow maps)
    R16_FLOAT,  ///< 16-bit float R (single channel data)
    R8_UNORM,   ///< 8-bit R (masks, single channel)

    // Compressed formats
    BC1_UNORM, ///< BC1 compression (DXT1)
    BC3_UNORM, ///< BC3 compression (DXT5)
    BC5_UNORM, ///< BC5 compression (normal maps)
    BC6H_UF16, ///< BC6H compression (HDR)
    BC7_UNORM, ///< BC7 compression (high quality)

    // Depth formats
    D24_UNORM_S8_UINT, ///< 24-bit depth + 8-bit stencil
    D32_FLOAT,         ///< 32-bit float depth
    D16_UNORM,         ///< 16-bit depth (shadow maps)

    // Special formats
    R11G11B10_FLOAT, ///< 11:11:10 float RGB (HDR without alpha)
    RGB10A2_UNORM    ///< 10:10:10:2 RGBA (high precision color)
};

/**
 * @brief Render target usage flags
 */
enum class RenderTargetUsage : uint32_t
{
    None = 0,
    RenderTarget = 1 << 0,    ///< Can be used as render target
    ShaderResource = 1 << 1,  ///< Can be used as shader resource
    DepthStencil = 1 << 2,    ///< Can be used as depth stencil
    UnorderedAccess = 1 << 3, ///< Can be used for compute shaders
    GenerateMips = 1 << 4,    ///< Auto-generate mipmaps
    CubeMap = 1 << 5,         ///< Cube map render target
    Array = 1 << 6,           ///< Texture array
    Multisampled = 1 << 7     ///< Multi-sampled render target
};

inline RenderTargetUsage operator|(RenderTargetUsage a, RenderTargetUsage b)
{
    return static_cast<RenderTargetUsage>(std::to_underlying(a) | std::to_underlying(b));
}

inline bool operator&(RenderTargetUsage a, RenderTargetUsage b)
{
    return (std::to_underlying(a) & std::to_underlying(b)) != 0;
}

/**
 * @brief Render target creation parameters
 */
struct RenderTargetDesc
{
    std::string name;                                            ///< Render target name
    uint32_t width = 1920;                                       ///< Width in pixels
    uint32_t height = 1080;                                      ///< Height in pixels
    uint32_t arraySize = 1;                                      ///< Array size (for texture arrays)
    uint32_t mipLevels = 1;                                      ///< Number of mip levels
    uint32_t sampleCount = 1;                                    ///< MSAA sample count
    uint32_t sampleQuality = 0;                                  ///< MSAA sample quality
    RenderTargetFormat format = RenderTargetFormat::RGBA8_UNORM; ///< Pixel format
    RenderTargetUsage usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource; ///< Usage flags
    XMFLOAT4 clearColor = {0, 0, 0, 1};                                                            ///< Clear color
    float clearDepth = 1.0f;  ///< Clear depth value
    uint8_t clearStencil = 0; ///< Clear stencil value
    bool autoClear = true;    ///< Automatically clear on bind
};

/**
 * @brief Individual render target implementation
 */
class RenderTarget
{
  public:
    RenderTarget(const RenderTargetDesc& desc);
    ~RenderTarget();

    /**
     * @brief Create DirectX resources
     */
    HRESULT Create(ID3D11Device* device);

    /**
     * @brief Destroy DirectX resources
     */
    void Destroy();

    /**
     * @brief Resize the render target
     */
    HRESULT Resize(ID3D11Device* device, uint32_t width, uint32_t height);

    /**
     * @brief Clear the render target
     */
    void Clear(ID3D11DeviceContext* context);

    /**
     * @brief Generate mipmaps (if supported)
     */
    void GenerateMips(ID3D11DeviceContext* context);

    // Getters
    const RenderTargetDesc& GetDesc() const { return m_desc; }
    RenderTargetDesc& GetDesc() { return m_desc; }
    ID3D11Texture2D* GetTexture() const { return m_texture.Get(); }
    ID3D11RenderTargetView* GetRenderTargetView() const { return m_renderTargetView.Get(); }
    ID3D11DepthStencilView* GetDepthStencilView() const { return m_depthStencilView.Get(); }
    ID3D11ShaderResourceView* GetShaderResourceView() const { return m_shaderResourceView.Get(); }
    ID3D11UnorderedAccessView* GetUnorderedAccessView() const { return m_unorderedAccessView.Get(); }

    bool IsValid() const { return m_texture != nullptr; }
    bool IsDepthStencil() const { return m_desc.usage & RenderTargetUsage::DepthStencil; }
    bool IsMultisampled() const { return m_desc.sampleCount > 1; }

    // Console integration
    std::string GetInfo() const;
    bool SaveToFile(const std::string& filename) const;

  private:
    RenderTargetDesc m_desc;
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
    ComPtr<ID3D11UnorderedAccessView> m_unorderedAccessView;

    DXGI_FORMAT GetDXGIFormat(RenderTargetFormat format) const;
    DXGI_FORMAT GetTypelessFormat(RenderTargetFormat format) const;
    DXGI_FORMAT GetSRVFormat(RenderTargetFormat format) const;
    DXGI_FORMAT GetDSVFormat(RenderTargetFormat format) const;

    // Helper for saving textures
    bool SaveBMP(const std::string& filename, unsigned char* data, uint32_t width, uint32_t height,
                 uint32_t pitch) const;
};

/**
 * @brief Multiple render targets (MRT) group
 */
class MultipleRenderTargets
{
  public:
    MultipleRenderTargets(const std::string& name);
    ~MultipleRenderTargets();

    /**
     * @brief Add a render target to the group
     */
    void AddRenderTarget(std::shared_ptr<RenderTarget> renderTarget, uint32_t slot = 0);

    /**
     * @brief Set depth stencil target
     */
    void SetDepthStencil(std::shared_ptr<RenderTarget> depthStencil);

    /**
     * @brief Create all render targets
     */
    HRESULT Create(ID3D11Device* device);

    /**
     * @brief Bind all render targets
     */
    void Bind(ID3D11DeviceContext* context);

    /**
     * @brief Unbind all render targets
     */
    void Unbind(ID3D11DeviceContext* context);

    /**
     * @brief Clear all render targets
     */
    void Clear(ID3D11DeviceContext* context);

    /**
     * @brief Resize all render targets
     */
    HRESULT Resize(ID3D11Device* device, uint32_t width, uint32_t height);

    // Getters
    const std::string& GetName() const { return m_name; }
    std::shared_ptr<RenderTarget> GetRenderTarget(uint32_t slot) const;
    std::shared_ptr<RenderTarget> GetDepthStencil() const { return m_depthStencil; }
    uint32_t GetRenderTargetCount() const { return static_cast<uint32_t>(m_renderTargets.size()); }

  private:
    std::string m_name;
    std::unordered_map<uint32_t, std::shared_ptr<RenderTarget>> m_renderTargets;
    std::shared_ptr<RenderTarget> m_depthStencil;
};

/**
 * @brief Render target manager for the graphics engine
 */
class RenderTargetManager
{
  public:
    /**
     * @brief Render target system metrics
     */
    struct RenderTargetMetrics
    {
        int totalRenderTargets;   ///< Total number of render targets
        int activeRenderTargets;  ///< Currently active render targets
        size_t totalMemoryUsage;  ///< Total memory usage in bytes
        size_t colorTargetMemory; ///< Color target memory usage
        size_t depthTargetMemory; ///< Depth target memory usage
        int mrtGroups;            ///< Number of MRT groups
        int resizeOperations;     ///< Number of resize operations
        float averageCreateTime;  ///< Average creation time in ms
        int failedCreations;      ///< Number of failed creations
    };

    RenderTargetManager();
    ~RenderTargetManager();

    /**
     * @brief Initialize the render target manager
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the render target manager
     */
    void Shutdown();

    // Render target management
    std::shared_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc);
    std::shared_ptr<RenderTarget> GetRenderTarget(const std::string& name) const;
    void DestroyRenderTarget(const std::string& name);

    // MRT management
    std::shared_ptr<MultipleRenderTargets> CreateMRT(const std::string& name);
    std::shared_ptr<MultipleRenderTargets> GetMRT(const std::string& name) const;
    void DestroyMRT(const std::string& name);

    // Predefined render target creation
    HRESULT CreateGBufferTargets(uint32_t width, uint32_t height, uint32_t sampleCount = 1);
    HRESULT CreateShadowMapTargets(uint32_t resolution = 2048);
    HRESULT CreatePostProcessTargets(uint32_t width, uint32_t height);
    HRESULT CreateTemporalTargets(uint32_t width, uint32_t height);

    // Global operations
    void ResizeAllTargets(uint32_t width, uint32_t height);
    void ClearAllTargets();

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Get render target metrics
     */
    RenderTargetMetrics Console_GetMetrics() const;

    /**
     * @brief List all render targets
     */
    std::string Console_ListRenderTargets() const;

    /**
     * @brief Get detailed render target information
     */
    std::string Console_GetRenderTargetInfo(const std::string& name) const;

    /**
     * @brief Save render target to file
     */
    bool Console_SaveRenderTarget(const std::string& name, const std::string& filename = "");

    /**
     * @brief Create render target via console
     */
    bool Console_CreateRenderTarget(const std::string& name, uint32_t width, uint32_t height,
                                    const std::string& format);

    /**
     * @brief Resize specific render target
     */
    bool Console_ResizeRenderTarget(const std::string& name, uint32_t width, uint32_t height);

    /**
     * @brief Toggle render target visualization
     */
    void Console_ToggleVisualization(const std::string& name);

    /**
     * @brief Set render target clear color
     */
    void Console_SetClearColor(const std::string& name, float r, float g, float b, float a);

    /**
     * @brief Force garbage collection of unused render targets
     */
    void Console_GarbageCollect();

    /**
     * @brief Validate all render targets
     */
    int Console_ValidateRenderTargets();

    /**
     * @brief Get memory usage breakdown
     */
    std::string Console_GetMemoryInfo() const;

    /**
     * @brief Render debug visualization of enabled render targets
     * Draws enabled render targets as small quads in the top-right corner.
     * @param context Device context for rendering
     * @param screenWidth Current screen width
     * @param screenHeight Current screen height
     */
    void RenderDebugVisualization(ID3D11DeviceContext* context, uint32_t screenWidth, uint32_t screenHeight);

    /**
     * @brief Check if a render target has visualization enabled
     */
    bool IsVisualizationEnabled(const std::string& name) const;

  private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;

    // Storage
    std::unordered_map<std::string, std::shared_ptr<RenderTarget>> m_renderTargets;
    std::unordered_map<std::string, std::shared_ptr<MultipleRenderTargets>> m_mrtGroups;

    // Metrics
    mutable std::mutex m_metricsMutex;
    RenderTargetMetrics m_metrics;

    // Debug visualization state (name -> enabled)
    std::unordered_map<std::string, bool> m_visualizationState;

    // Helper methods
    void UpdateMetrics();
    size_t CalculateMemoryUsage(const RenderTargetDesc& desc) const;
    uint32_t GetFormatSize(RenderTargetFormat format) const;
    std::string FormatToString(RenderTargetFormat format) const;
    RenderTargetFormat StringToFormat(const std::string& str) const;
};
