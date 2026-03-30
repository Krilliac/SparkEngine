#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file RenderTargetManager.cpp
 * @brief RenderTargetManager core implementation — lifecycle, RT/MRT CRUD,
 *        predefined target creation (GBuffer, shadow, post-process, temporal),
 *        global resize/clear, metrics, and format utility helpers.
 *
 * Console integration methods live in RenderTargetConsole.cpp.
 *
 * @author Spark Engine Team
 * @date 2025
 */

#include "RenderTarget.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// RENDER TARGET MANAGER — LIFECYCLE
// ============================================================================

RenderTargetManager::RenderTargetManager() : m_device(nullptr), m_context(nullptr)
{
    m_metrics = {};
}

RenderTargetManager::~RenderTargetManager()
{
    Shutdown();
}

HRESULT RenderTargetManager::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Initializing RenderTargetManager");
    m_device = device;
    m_context = context;

    // Create default render targets would go here

    return S_OK;
}

void RenderTargetManager::Shutdown()
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shutting down RenderTargetManager (%zu targets, %zu MRT groups)",
                   m_renderTargets.size(), m_mrtGroups.size());
    m_mrtGroups.clear();
    m_renderTargets.clear();
    m_device = nullptr;
    m_context = nullptr;
}

// ============================================================================
// RENDER TARGET MANAGER — RT/MRT CRUD
// ============================================================================

std::shared_ptr<RenderTarget> RenderTargetManager::CreateRenderTarget(const RenderTargetDesc& desc)
{
    auto renderTarget = std::make_shared<RenderTarget>(desc);

    if (m_device && SUCCEEDED(renderTarget->Create(m_device)))
    {
        m_renderTargets[desc.name] = renderTarget;
        UpdateMetrics();
        return renderTarget;
    }

    return nullptr;
}

std::shared_ptr<RenderTarget> RenderTargetManager::GetRenderTarget(const std::string& name) const
{
    auto it = m_renderTargets.find(name);
    return (it != m_renderTargets.end()) ? it->second : nullptr;
}

void RenderTargetManager::DestroyRenderTarget(const std::string& name)
{
    auto it = m_renderTargets.find(name);
    if (it != m_renderTargets.end())
    {
        it->second->Destroy();
        m_renderTargets.erase(it);
        UpdateMetrics();
    }
}

std::shared_ptr<MultipleRenderTargets> RenderTargetManager::CreateMRT(const std::string& name)
{
    auto mrt = std::make_shared<MultipleRenderTargets>(name);
    m_mrtGroups[name] = mrt;
    return mrt;
}

std::shared_ptr<MultipleRenderTargets> RenderTargetManager::GetMRT(const std::string& name) const
{
    auto it = m_mrtGroups.find(name);
    return (it != m_mrtGroups.end()) ? it->second : nullptr;
}

void RenderTargetManager::DestroyMRT(const std::string& name)
{
    m_mrtGroups.erase(name);
}

// ============================================================================
// RENDER TARGET MANAGER — PREDEFINED TARGETS
// ============================================================================

HRESULT RenderTargetManager::CreateGBufferTargets(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Creating GBuffer targets (%ux%u, %ux MSAA)", width, height,
                   sampleCount);
    // G-Buffer for deferred rendering

    // Albedo + Metallic
    RenderTargetDesc albedoDesc;
    albedoDesc.name = "GBuffer_Albedo";
    albedoDesc.width = width;
    albedoDesc.height = height;
    albedoDesc.format = RenderTargetFormat::RGBA8_SRGB;
    albedoDesc.sampleCount = sampleCount;
    albedoDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(albedoDesc);

    // Normal + Roughness
    RenderTargetDesc normalDesc;
    normalDesc.name = "GBuffer_Normal";
    normalDesc.width = width;
    normalDesc.height = height;
    normalDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    normalDesc.sampleCount = sampleCount;
    normalDesc.clearColor = {0.5f, 0.5f, 1.0f, 1.0f};
    CreateRenderTarget(normalDesc);

    // Motion vectors
    RenderTargetDesc motionDesc;
    motionDesc.name = "GBuffer_Motion";
    motionDesc.width = width;
    motionDesc.height = height;
    motionDesc.format = RenderTargetFormat::RG16_FLOAT;
    motionDesc.sampleCount = sampleCount;
    motionDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(motionDesc);

    // Depth buffer
    RenderTargetDesc depthDesc;
    depthDesc.name = "GBuffer_Depth";
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = RenderTargetFormat::D24_UNORM_S8_UINT;
    depthDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
    depthDesc.sampleCount = sampleCount;
    depthDesc.clearDepth = 1.0f;
    depthDesc.clearStencil = 0;
    CreateRenderTarget(depthDesc);

    return S_OK;
}

HRESULT RenderTargetManager::CreateShadowMapTargets(uint32_t resolution)
{
    // Cascaded shadow maps
    for (int i = 0; i < 4; i++)
    {
        RenderTargetDesc shadowDesc;
        shadowDesc.name = "ShadowMap_Cascade" + std::to_string(i);
        shadowDesc.width = resolution;
        shadowDesc.height = resolution;
        shadowDesc.format = RenderTargetFormat::D32_FLOAT;
        shadowDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
        shadowDesc.clearDepth = 1.0f;
        CreateRenderTarget(shadowDesc);
    }

    return S_OK;
}

HRESULT RenderTargetManager::CreatePostProcessTargets(uint32_t width, uint32_t height)
{
    // HDR render target
    RenderTargetDesc hdrDesc;
    hdrDesc.name = "PostProcess_HDR";
    hdrDesc.width = width;
    hdrDesc.height = height;
    hdrDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    hdrDesc.usage =
        RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource | RenderTargetUsage::GenerateMips;
    hdrDesc.mipLevels = 0; // Full mip chain
    hdrDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    CreateRenderTarget(hdrDesc);

    // Bloom targets
    for (int i = 0; i < 6; i++)
    {
        uint32_t mipWidth = width >> (i + 1);
        uint32_t mipHeight = height >> (i + 1);

        RenderTargetDesc bloomDesc;
        bloomDesc.name = "PostProcess_Bloom" + std::to_string(i);
        bloomDesc.width = mipWidth;
        bloomDesc.height = mipHeight;
        bloomDesc.format = RenderTargetFormat::RGBA16_FLOAT;
        bloomDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        CreateRenderTarget(bloomDesc);
    }

    return S_OK;
}

HRESULT RenderTargetManager::CreateTemporalTargets(uint32_t width, uint32_t height)
{
    // TAA history buffer
    RenderTargetDesc historyDesc;
    historyDesc.name = "Temporal_History";
    historyDesc.width = width;
    historyDesc.height = height;
    historyDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    historyDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    CreateRenderTarget(historyDesc);

    // Velocity buffer
    RenderTargetDesc velocityDesc;
    velocityDesc.name = "Temporal_Velocity";
    velocityDesc.width = width;
    velocityDesc.height = height;
    velocityDesc.format = RenderTargetFormat::RG16_FLOAT;
    velocityDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(velocityDesc);

    return S_OK;
}

// ============================================================================
// RENDER TARGET MANAGER — GLOBAL OPERATIONS
// ============================================================================

void RenderTargetManager::ResizeAllTargets(uint32_t width, uint32_t height)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Resizing all %zu render targets to %ux%u", m_renderTargets.size(),
                   width, height);
    for (auto& pair : m_renderTargets)
    {
        pair.second->Resize(m_device, width, height);
    }
    UpdateMetrics();
}

void RenderTargetManager::ClearAllTargets()
{
    if (!m_context)
        return;

    for (auto& pair : m_renderTargets)
    {
        pair.second->Clear(m_context);
    }
}

// ============================================================================
// RENDER TARGET MANAGER — METRICS & FORMAT HELPERS
// ============================================================================

void RenderTargetManager::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);

    m_metrics.totalRenderTargets = static_cast<int>(m_renderTargets.size());
    m_metrics.mrtGroups = static_cast<int>(m_mrtGroups.size());

    // Calculate memory usage
    m_metrics.totalMemoryUsage = 0;
    for (const auto& pair : m_renderTargets)
    {
        m_metrics.totalMemoryUsage += CalculateMemoryUsage(pair.second->GetDesc());
    }
}

size_t RenderTargetManager::CalculateMemoryUsage(const RenderTargetDesc& desc) const
{
    uint32_t formatSize = GetFormatSize(desc.format);
    return static_cast<size_t>(desc.width) * desc.height * desc.arraySize * formatSize * desc.sampleCount;
}

uint32_t RenderTargetManager::GetFormatSize(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::RGBA8_UNORM:
    case RenderTargetFormat::RGBA8_SRGB:
    case RenderTargetFormat::RGB10A2_UNORM:
    case RenderTargetFormat::R11G11B10_FLOAT:
    case RenderTargetFormat::RG16_FLOAT:
    case RenderTargetFormat::R32_FLOAT:
    case RenderTargetFormat::D24_UNORM_S8_UINT:
    case RenderTargetFormat::D32_FLOAT:
        return 4;
    case RenderTargetFormat::RGBA16_FLOAT:
    case RenderTargetFormat::RG32_FLOAT:
        return 8;
    case RenderTargetFormat::RGBA32_FLOAT:
        return 16;
    case RenderTargetFormat::R16_FLOAT:
    case RenderTargetFormat::D16_UNORM:
        return 2;
    case RenderTargetFormat::R8_UNORM:
        return 1;
    default:
        return 4;
    }
}

std::string RenderTargetManager::FormatToString(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::RGBA8_UNORM:
        return "RGBA8_UNORM";
    case RenderTargetFormat::RGBA8_SRGB:
        return "RGBA8_SRGB";
    case RenderTargetFormat::RGBA16_FLOAT:
        return "RGBA16_FLOAT";
    case RenderTargetFormat::RGBA32_FLOAT:
        return "RGBA32_FLOAT";
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case RenderTargetFormat::D32_FLOAT:
        return "D32_FLOAT";
    default:
        return "UNKNOWN";
    }
}

RenderTargetFormat RenderTargetManager::StringToFormat(const std::string& str) const
{
    if (str == "RGBA8_UNORM")
        return RenderTargetFormat::RGBA8_UNORM;
    if (str == "RGBA8_SRGB")
        return RenderTargetFormat::RGBA8_SRGB;
    if (str == "RGBA16_FLOAT")
        return RenderTargetFormat::RGBA16_FLOAT;
    if (str == "RGBA32_FLOAT")
        return RenderTargetFormat::RGBA32_FLOAT;
    if (str == "D24_UNORM_S8_UINT")
        return RenderTargetFormat::D24_UNORM_S8_UINT;
    if (str == "D32_FLOAT")
        return RenderTargetFormat::D32_FLOAT;
    return RenderTargetFormat::RGBA8_UNORM; // Default
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "RenderTarget.h"
#include "../Utils/Validate.h"
#include <cstring>

// ============================================================================
// RenderTargetManager (Linux stub) — Core
// ============================================================================

RenderTargetManager::RenderTargetManager() : m_device(nullptr), m_context(nullptr)
{
    memset(&m_metrics, 0, sizeof(m_metrics));
}

RenderTargetManager::~RenderTargetManager()
{
    Shutdown();
}

HRESULT RenderTargetManager::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    return S_OK;
}

void RenderTargetManager::Shutdown()
{
    m_mrtGroups.clear();
    m_renderTargets.clear();
    m_device = nullptr;
    m_context = nullptr;
}

std::shared_ptr<RenderTarget> RenderTargetManager::CreateRenderTarget(const RenderTargetDesc& desc)
{
    auto renderTarget = std::make_shared<RenderTarget>(desc);
    renderTarget->Create(m_device);
    m_renderTargets[desc.name] = renderTarget;
    UpdateMetrics();
    return renderTarget;
}

std::shared_ptr<RenderTarget> RenderTargetManager::GetRenderTarget(const std::string& name) const
{
    auto it = m_renderTargets.find(name);
    return (it != m_renderTargets.end()) ? it->second : nullptr;
}

void RenderTargetManager::DestroyRenderTarget(const std::string& name)
{
    auto it = m_renderTargets.find(name);
    if (it != m_renderTargets.end())
    {
        it->second->Destroy();
        m_renderTargets.erase(it);
        UpdateMetrics();
    }
}

std::shared_ptr<MultipleRenderTargets> RenderTargetManager::CreateMRT(const std::string& name)
{
    auto mrt = std::make_shared<MultipleRenderTargets>(name);
    m_mrtGroups[name] = mrt;
    return mrt;
}

std::shared_ptr<MultipleRenderTargets> RenderTargetManager::GetMRT(const std::string& name) const
{
    auto it = m_mrtGroups.find(name);
    return (it != m_mrtGroups.end()) ? it->second : nullptr;
}

void RenderTargetManager::DestroyMRT(const std::string& name)
{
    m_mrtGroups.erase(name);
}

HRESULT RenderTargetManager::CreateGBufferTargets(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    RenderTargetDesc albedoDesc;
    albedoDesc.name = "GBuffer_Albedo";
    albedoDesc.width = width;
    albedoDesc.height = height;
    albedoDesc.format = RenderTargetFormat::RGBA8_SRGB;
    albedoDesc.sampleCount = sampleCount;
    albedoDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(albedoDesc);

    RenderTargetDesc normalDesc;
    normalDesc.name = "GBuffer_Normal";
    normalDesc.width = width;
    normalDesc.height = height;
    normalDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    normalDesc.sampleCount = sampleCount;
    normalDesc.clearColor = {0.5f, 0.5f, 1.0f, 1.0f};
    CreateRenderTarget(normalDesc);

    RenderTargetDesc motionDesc;
    motionDesc.name = "GBuffer_Motion";
    motionDesc.width = width;
    motionDesc.height = height;
    motionDesc.format = RenderTargetFormat::RG16_FLOAT;
    motionDesc.sampleCount = sampleCount;
    motionDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(motionDesc);

    RenderTargetDesc depthDesc;
    depthDesc.name = "GBuffer_Depth";
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = RenderTargetFormat::D24_UNORM_S8_UINT;
    depthDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
    depthDesc.sampleCount = sampleCount;
    depthDesc.clearDepth = 1.0f;
    depthDesc.clearStencil = 0;
    CreateRenderTarget(depthDesc);

    return S_OK;
}

HRESULT RenderTargetManager::CreateShadowMapTargets(uint32_t resolution)
{
    for (int i = 0; i < 4; i++)
    {
        RenderTargetDesc shadowDesc;
        shadowDesc.name = "ShadowMap_Cascade" + std::to_string(i);
        shadowDesc.width = resolution;
        shadowDesc.height = resolution;
        shadowDesc.format = RenderTargetFormat::D32_FLOAT;
        shadowDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
        shadowDesc.clearDepth = 1.0f;
        CreateRenderTarget(shadowDesc);
    }
    return S_OK;
}

HRESULT RenderTargetManager::CreatePostProcessTargets(uint32_t width, uint32_t height)
{
    RenderTargetDesc hdrDesc;
    hdrDesc.name = "PostProcess_HDR";
    hdrDesc.width = width;
    hdrDesc.height = height;
    hdrDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    hdrDesc.usage =
        RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource | RenderTargetUsage::GenerateMips;
    hdrDesc.mipLevels = 0;
    hdrDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    CreateRenderTarget(hdrDesc);

    for (int i = 0; i < 6; i++)
    {
        uint32_t mipWidth = width >> (i + 1);
        uint32_t mipHeight = height >> (i + 1);
        RenderTargetDesc bloomDesc;
        bloomDesc.name = "PostProcess_Bloom" + std::to_string(i);
        bloomDesc.width = mipWidth;
        bloomDesc.height = mipHeight;
        bloomDesc.format = RenderTargetFormat::RGBA16_FLOAT;
        bloomDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        CreateRenderTarget(bloomDesc);
    }

    return S_OK;
}

HRESULT RenderTargetManager::CreateTemporalTargets(uint32_t width, uint32_t height)
{
    RenderTargetDesc historyDesc;
    historyDesc.name = "Temporal_History";
    historyDesc.width = width;
    historyDesc.height = height;
    historyDesc.format = RenderTargetFormat::RGBA16_FLOAT;
    historyDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    CreateRenderTarget(historyDesc);

    RenderTargetDesc velocityDesc;
    velocityDesc.name = "Temporal_Velocity";
    velocityDesc.width = width;
    velocityDesc.height = height;
    velocityDesc.format = RenderTargetFormat::RG16_FLOAT;
    velocityDesc.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    CreateRenderTarget(velocityDesc);

    return S_OK;
}

void RenderTargetManager::ResizeAllTargets(uint32_t width, uint32_t height)
{
    for (auto& pair : m_renderTargets)
    {
        pair.second->Resize(m_device, width, height);
    }
    UpdateMetrics();
}

void RenderTargetManager::ClearAllTargets()
{
    if (!m_context)
        return;
    for (auto& pair : m_renderTargets)
    {
        pair.second->Clear(m_context);
    }
}

void RenderTargetManager::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalRenderTargets = static_cast<int>(m_renderTargets.size());
    m_metrics.mrtGroups = static_cast<int>(m_mrtGroups.size());
    m_metrics.totalMemoryUsage = 0;
    for (const auto& pair : m_renderTargets)
    {
        m_metrics.totalMemoryUsage += CalculateMemoryUsage(pair.second->GetDesc());
    }
}

size_t RenderTargetManager::CalculateMemoryUsage(const RenderTargetDesc& desc) const
{
    uint32_t formatSize = GetFormatSize(desc.format);
    return static_cast<size_t>(desc.width) * desc.height * desc.arraySize * formatSize * desc.sampleCount;
}

uint32_t RenderTargetManager::GetFormatSize(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::RGBA8_UNORM:
    case RenderTargetFormat::RGBA8_SRGB:
    case RenderTargetFormat::RGB10A2_UNORM:
    case RenderTargetFormat::R11G11B10_FLOAT:
    case RenderTargetFormat::RG16_FLOAT:
    case RenderTargetFormat::R32_FLOAT:
    case RenderTargetFormat::D24_UNORM_S8_UINT:
    case RenderTargetFormat::D32_FLOAT:
        return 4;
    case RenderTargetFormat::RGBA16_FLOAT:
    case RenderTargetFormat::RG32_FLOAT:
        return 8;
    case RenderTargetFormat::RGBA32_FLOAT:
        return 16;
    case RenderTargetFormat::R16_FLOAT:
    case RenderTargetFormat::D16_UNORM:
        return 2;
    case RenderTargetFormat::R8_UNORM:
        return 1;
    default:
        return 4;
    }
}

std::string RenderTargetManager::FormatToString(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::RGBA8_UNORM:
        return "RGBA8_UNORM";
    case RenderTargetFormat::RGBA8_SRGB:
        return "RGBA8_SRGB";
    case RenderTargetFormat::RGBA16_FLOAT:
        return "RGBA16_FLOAT";
    case RenderTargetFormat::RGBA32_FLOAT:
        return "RGBA32_FLOAT";
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case RenderTargetFormat::D32_FLOAT:
        return "D32_FLOAT";
    default:
        return "UNKNOWN";
    }
}

RenderTargetFormat RenderTargetManager::StringToFormat(const std::string& str) const
{
    if (str == "RGBA8_UNORM")
        return RenderTargetFormat::RGBA8_UNORM;
    if (str == "RGBA8_SRGB")
        return RenderTargetFormat::RGBA8_SRGB;
    if (str == "RGBA16_FLOAT")
        return RenderTargetFormat::RGBA16_FLOAT;
    if (str == "RGBA32_FLOAT")
        return RenderTargetFormat::RGBA32_FLOAT;
    if (str == "D24_UNORM_S8_UINT")
        return RenderTargetFormat::D24_UNORM_S8_UINT;
    if (str == "D32_FLOAT")
        return RenderTargetFormat::D32_FLOAT;
    return RenderTargetFormat::RGBA8_UNORM;
}

#endif // SPARK_PLATFORM_WINDOWS
