#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file RenderTarget.cpp
 * @brief Implementation of render target wrapper for DirectX 11
 * @author Spark Engine Team
 * @date 2025
 */

#include "RenderTarget.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// RENDERTARGET IMPLEMENTATION
// ============================================================================

RenderTarget::RenderTarget(const RenderTargetDesc& desc) : m_desc(desc) {}

RenderTarget::~RenderTarget()
{
    // COM objects are automatically released by ComPtr
}

HRESULT RenderTarget::Create(ID3D11Device* device)
{
    ASSERT(device);

    DXGI_FORMAT dxgiFormat = GetDXGIFormat(m_desc.format);

    // Create texture
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = m_desc.width;
    textureDesc.Height = m_desc.height;
    textureDesc.MipLevels = m_desc.mipLevels;
    textureDesc.ArraySize = m_desc.arraySize;
    textureDesc.Format = GetTypelessFormat(m_desc.format);
    textureDesc.SampleDesc.Count = m_desc.sampleCount;
    textureDesc.SampleDesc.Quality = m_desc.sampleQuality;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;

    // Set bind flags based on usage
    UINT bindFlags = 0;
    if (m_desc.usage & RenderTargetUsage::RenderTarget)
    {
        bindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    if (m_desc.usage & RenderTargetUsage::DepthStencil)
    {
        bindFlags |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (m_desc.usage & RenderTargetUsage::ShaderResource)
    {
        bindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (m_desc.usage & RenderTargetUsage::UnorderedAccess)
    {
        bindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    textureDesc.BindFlags = bindFlags;

    HRESULT hr = device->CreateTexture2D(&textureDesc, nullptr, &m_texture);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().Log("Failed to create render target texture: " + m_desc.name, "ERROR");
        return hr;
    }

    // Create render target view (if not depth buffer)
    if (m_desc.usage & RenderTargetUsage::RenderTarget)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = dxgiFormat;
        rtvDesc.ViewDimension =
            (m_desc.sampleCount > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

        hr = device->CreateRenderTargetView(m_texture.Get(), &rtvDesc, &m_renderTargetView);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().Log("Failed to create render target view: " + m_desc.name, "ERROR");
            return hr;
        }
    }

    // Create depth stencil view
    if (m_desc.usage & RenderTargetUsage::DepthStencil)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = GetDSVFormat(m_desc.format);
        dsvDesc.ViewDimension =
            (m_desc.sampleCount > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;

        hr = device->CreateDepthStencilView(m_texture.Get(), &dsvDesc, &m_depthStencilView);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().Log("Failed to create depth stencil view: " + m_desc.name, "ERROR");
            return hr;
        }
    }

    // Create shader resource view
    if (m_desc.usage & RenderTargetUsage::ShaderResource)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = GetSRVFormat(m_desc.format);
        srvDesc.ViewDimension =
            (m_desc.sampleCount > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = m_desc.mipLevels;

        hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_shaderResourceView);
        if (FAILED(hr))
        {
            Spark::SimpleConsole::GetInstance().Log("Failed to create shader resource view: " + m_desc.name, "ERROR");
            return hr;
        }
    }

    std::string successMsg = "RenderTarget '" + m_desc.name + "' created successfully (" +
                             std::to_string(m_desc.width) + "x" + std::to_string(m_desc.height) + ")";
    Spark::SimpleConsole::GetInstance().Log(successMsg, "SUCCESS");

    return S_OK;
}

void RenderTarget::Destroy()
{
    m_unorderedAccessView.Reset();
    m_shaderResourceView.Reset();
    m_depthStencilView.Reset();
    m_renderTargetView.Reset();
    m_texture.Reset();
}

HRESULT RenderTarget::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_desc.width = width;
    m_desc.height = height;

    Destroy();
    return Create(device);
}

void RenderTarget::Clear(ID3D11DeviceContext* context)
{
    ASSERT(context);

    if (m_renderTargetView)
    {
        context->ClearRenderTargetView(m_renderTargetView.Get(), &m_desc.clearColor.x);
    }

    if (m_depthStencilView)
    {
        context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                       m_desc.clearDepth, m_desc.clearStencil);
    }
}

void RenderTarget::GenerateMips(ID3D11DeviceContext* context)
{
    if (m_shaderResourceView && (m_desc.usage & RenderTargetUsage::GenerateMips))
    {
        context->GenerateMips(m_shaderResourceView.Get());
    }
}

DXGI_FORMAT RenderTarget::GetDXGIFormat(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::RGBA8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RenderTargetFormat::RGBA8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case RenderTargetFormat::RGBA16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RenderTargetFormat::RGBA32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RenderTargetFormat::RG16_FLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;
    case RenderTargetFormat::RG32_FLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;
    case RenderTargetFormat::R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case RenderTargetFormat::R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case RenderTargetFormat::R8_UNORM:
        return DXGI_FORMAT_R8_UNORM;
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RenderTargetFormat::D32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case RenderTargetFormat::D16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    case RenderTargetFormat::R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case RenderTargetFormat::RGB10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

DXGI_FORMAT RenderTarget::GetTypelessFormat(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case RenderTargetFormat::D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case RenderTargetFormat::D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return GetDXGIFormat(format);
    }
}

DXGI_FORMAT RenderTarget::GetSRVFormat(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case RenderTargetFormat::D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case RenderTargetFormat::D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return GetDXGIFormat(format);
    }
}

DXGI_FORMAT RenderTarget::GetDSVFormat(RenderTargetFormat format) const
{
    switch (format)
    {
    case RenderTargetFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RenderTargetFormat::D32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case RenderTargetFormat::D16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    default:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
}

std::string RenderTarget::GetInfo() const
{
    std::string info = "RenderTarget: " + m_desc.name + "\n";
    info += "Size: " + std::to_string(m_desc.width) + "x" + std::to_string(m_desc.height) + "\n";
    info += "Array Size: " + std::to_string(m_desc.arraySize) + "\n";
    info += "Mip Levels: " + std::to_string(m_desc.mipLevels) + "\n";
    info += "Sample Count: " + std::to_string(m_desc.sampleCount) + "\n";
    info += "Valid: " + std::string(IsValid() ? "Yes" : "No") + "\n";
    info += "Depth/Stencil: " + std::string(IsDepthStencil() ? "Yes" : "No") + "\n";
    info += "Multisampled: " + std::string(IsMultisampled() ? "Yes" : "No") + "\n";
    return info;
}

bool RenderTarget::SaveToFile(const std::string& filename) const
{
    if (!m_texture)
    {
        return false;
    }

    // Create staging texture for reading
    D3D11_TEXTURE2D_DESC desc;
    m_texture->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Device> device;
    m_texture->GetDevice(&device);

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &stagingTexture);
    if (FAILED(hr))
    {
        return false;
    }

    // Copy texture to staging
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(stagingTexture.Get(), m_texture.Get());

    // Map and read data
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        return false;
    }

    // Simple BMP save implementation (basic functionality)
    bool result =
        SaveBMP(filename, static_cast<unsigned char*>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch);

    context->Unmap(stagingTexture.Get(), 0);
    return result;
}

bool RenderTarget::SaveBMP(const std::string& filename, unsigned char* data, uint32_t width, uint32_t height,
                           uint32_t pitch) const
{
    // Basic BMP file header
    struct BMPHeader
    {
        char signature[2] = {'B', 'M'};
        uint32_t fileSize;
        uint32_t reserved = 0;
        uint32_t dataOffset = 54;
        uint32_t headerSize = 40;
        uint32_t width;
        uint32_t height;
        uint16_t planes = 1;
        uint16_t bitsPerPixel = 24;
        uint32_t compression = 0;
        uint32_t imageSize;
        uint32_t xPixelsPerMeter = 2835;
        uint32_t yPixelsPerMeter = 2835;
        uint32_t colorsUsed = 0;
        uint32_t importantColors = 0;
    };

    BMPHeader header;
    header.width = width;
    header.height = height;
    header.imageSize = width * height * 3;
    header.fileSize = header.dataOffset + header.imageSize;

    FILE* file = fopen(filename.c_str(), "wb");
    if (!file)
    {
        return false;
    }

    // Write header
    fwrite(&header, sizeof(BMPHeader), 1, file);

    // Write pixel data (convert RGBA to BGR)
    for (uint32_t y = 0; y < height; y++)
    {
        unsigned char* row = data + (height - 1 - y) * pitch; // Flip Y
        for (uint32_t x = 0; x < width; x++)
        {
            unsigned char bgr[3] = {row[2], row[1], row[0]}; // RGBA -> BGR
            fwrite(bgr, 3, 1, file);
            row += 4; // Skip alpha
        }
    }

    fclose(file);
    return true;
}

// ============================================================================
// MULTIPLE RENDER TARGETS IMPLEMENTATION
// ============================================================================

MultipleRenderTargets::MultipleRenderTargets(const std::string& name) : m_name(name) {}

MultipleRenderTargets::~MultipleRenderTargets() {}

void MultipleRenderTargets::AddRenderTarget(std::shared_ptr<RenderTarget> renderTarget, uint32_t slot)
{
    m_renderTargets[slot] = renderTarget;
}

void MultipleRenderTargets::SetDepthStencil(std::shared_ptr<RenderTarget> depthStencil)
{
    m_depthStencil = depthStencil;
}

HRESULT MultipleRenderTargets::Create(ID3D11Device* device)
{
    HRESULT hr = S_OK;

    for (auto& pair : m_renderTargets)
    {
        hr = pair.second->Create(device);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    if (m_depthStencil)
    {
        hr = m_depthStencil->Create(device);
    }

    return hr;
}

void MultipleRenderTargets::Bind(ID3D11DeviceContext* context)
{
    constexpr uint32_t maxRenderTargets = 8;
    ID3D11RenderTargetView* renderTargets[maxRenderTargets] = {};

    for (auto& pair : m_renderTargets)
    {
        if (pair.first < maxRenderTargets)
        {
            renderTargets[pair.first] = pair.second->GetRenderTargetView();
        }
    }

    ID3D11DepthStencilView* depthStencil = nullptr;
    if (m_depthStencil)
    {
        depthStencil = m_depthStencil->GetDepthStencilView();
    }

    context->OMSetRenderTargets(maxRenderTargets, renderTargets, depthStencil);
}

void MultipleRenderTargets::Unbind(ID3D11DeviceContext* context)
{
    constexpr uint32_t maxRenderTargets = 8;
    ID3D11RenderTargetView* nullRenderTargets[maxRenderTargets] = {};
    context->OMSetRenderTargets(maxRenderTargets, nullRenderTargets, nullptr);
}

void MultipleRenderTargets::Clear(ID3D11DeviceContext* context)
{
    for (auto& pair : m_renderTargets)
    {
        pair.second->Clear(context);
    }

    if (m_depthStencil)
    {
        m_depthStencil->Clear(context);
    }
}

HRESULT MultipleRenderTargets::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    HRESULT hr = S_OK;

    for (auto& pair : m_renderTargets)
    {
        hr = pair.second->Resize(device, width, height);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    if (m_depthStencil)
    {
        hr = m_depthStencil->Resize(device, width, height);
    }

    return hr;
}

std::shared_ptr<RenderTarget> MultipleRenderTargets::GetRenderTarget(uint32_t slot) const
{
    auto it = m_renderTargets.find(slot);
    return (it != m_renderTargets.end()) ? it->second : nullptr;
}

// ============================================================================
// RENDER TARGET MANAGER IMPLEMENTATION
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
    m_device = device;
    m_context = context;

    // Create default render targets would go here

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

// Console methods implementation would go here...
RenderTargetManager::RenderTargetMetrics RenderTargetManager::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// Additional helper implementations...
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

// Additional implementations for remaining manager methods would continue here...

HRESULT RenderTargetManager::CreateGBufferTargets(uint32_t width, uint32_t height, uint32_t sampleCount)
{
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

std::string RenderTargetManager::Console_ListRenderTargets() const
{
    std::string result = "=== Render Targets ===\n";

    for (const auto& pair : m_renderTargets)
    {
        const auto& desc = pair.second->GetDesc();
        result += pair.first + " (" + std::to_string(desc.width) + "x" + std::to_string(desc.height) + ", " +
                  FormatToString(desc.format) + ")\n";
    }

    result += "\n=== MRT Groups ===\n";
    for (const auto& pair : m_mrtGroups)
    {
        result += pair.first + " (" + std::to_string(pair.second->GetRenderTargetCount()) + " targets)\n";
    }

    return result;
}

std::string RenderTargetManager::Console_GetRenderTargetInfo(const std::string& name) const
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        return rt->GetInfo();
    }
    return "Render target '" + name + "' not found";
}

bool RenderTargetManager::Console_SaveRenderTarget(const std::string& name, const std::string& filename)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
    {
        return false;
    }

    std::string actualFilename = filename.empty() ? (name + ".bmp") : filename;
    return rt->SaveToFile(actualFilename);
}

bool RenderTargetManager::Console_CreateRenderTarget(const std::string& name, uint32_t width, uint32_t height,
                                                     const std::string& format)
{
    RenderTargetDesc desc;
    desc.name = name;
    desc.width = width;
    desc.height = height;
    desc.format = StringToFormat(format);

    auto rt = CreateRenderTarget(desc);
    return rt != nullptr;
}

bool RenderTargetManager::Console_ResizeRenderTarget(const std::string& name, uint32_t width, uint32_t height)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
    {
        return false;
    }

    return SUCCEEDED(rt->Resize(m_device, width, height));
}

void RenderTargetManager::Console_ToggleVisualization(const std::string& name)
{
    // Toggle debug visualization for a specific render target
    // When enabled, the render target will be drawn as a screen-space quad overlay
    auto it = m_visualizationState.find(name);
    if (it != m_visualizationState.end())
    {
        it->second = !it->second;
    }
    else
    {
        // Verify render target exists before adding
        auto rt = GetRenderTarget(name);
        if (rt)
        {
            m_visualizationState[name] = true;
        }
    }
}

void RenderTargetManager::RenderDebugVisualization(ID3D11DeviceContext* context, uint32_t screenWidth,
                                                   uint32_t screenHeight)
{
    if (!context || m_visualizationState.empty())
        return;

    // Calculate grid layout for debug quads
    int activeCount = 0;
    for (const auto& pair : m_visualizationState)
        if (pair.second)
            activeCount++;

    if (activeCount == 0)
        return;

    int cols = static_cast<int>(ceilf(sqrtf(static_cast<float>(activeCount))));
    int rows = (activeCount + cols - 1) / cols;

    float quadWidth = screenWidth / static_cast<float>(cols) * 0.25f;
    float quadHeight = screenHeight / static_cast<float>(rows) * 0.25f;

    int index = 0;
    for (const auto& pair : m_visualizationState)
    {
        if (!pair.second)
            continue;

        auto rt = GetRenderTarget(pair.first);
        if (!rt || !rt->GetShaderResourceView())
            continue;

        // Calculate screen position for this quad (top-right corner)
        float x = screenWidth - (cols - (index % cols)) * quadWidth;
        float y = (index / cols) * quadHeight;

        // Bind the render target's SRV for debug rendering
        // The actual quad rendering would be done by the graphics engine's
        // fullscreen quad shader with the viewport set to the small debug region
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = x;
        vp.TopLeftY = y;
        vp.Width = quadWidth;
        vp.Height = quadHeight;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        ID3D11ShaderResourceView* srv = rt->GetShaderResourceView();
        context->PSSetShaderResources(0, 1, &srv);

        // Note: caller is responsible for binding the debug visualization shader
        // and issuing the draw call (e.g., fullscreen triangle)

        index++;
    }

    // Restore the full-screen viewport
    D3D11_VIEWPORT fullVp = {};
    fullVp.Width = static_cast<float>(screenWidth);
    fullVp.Height = static_cast<float>(screenHeight);
    fullVp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &fullVp);

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

bool RenderTargetManager::IsVisualizationEnabled(const std::string& name) const
{
    auto it = m_visualizationState.find(name);
    return it != m_visualizationState.end() && it->second;
}

void RenderTargetManager::Console_SetClearColor(const std::string& name, float r, float g, float b, float a)
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        auto& desc = const_cast<RenderTargetDesc&>(rt->GetDesc());
        desc.clearColor = {r, g, b, a};
    }
}

void RenderTargetManager::Console_GarbageCollect()
{
    // Remove unused render targets
    for (auto it = m_renderTargets.begin(); it != m_renderTargets.end();)
    {
        if (it->second.use_count() == 1)
        { // Only held by manager
            it->second->Destroy();
            it = m_renderTargets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    UpdateMetrics();
}

int RenderTargetManager::Console_ValidateRenderTargets()
{
    int errors = 0;

    for (const auto& pair : m_renderTargets)
    {
        if (!pair.second->IsValid())
        {
            errors++;
        }
    }

    return errors;
}

std::string RenderTargetManager::Console_GetMemoryInfo() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);

    std::string result = "=== Render Target Memory Usage ===\n";
    result += "Total Memory: " + std::to_string(m_metrics.totalMemoryUsage / 1024 / 1024) + " MB\n";
    result += "Color Targets: " + std::to_string(m_metrics.colorTargetMemory / 1024 / 1024) + " MB\n";
    result += "Depth Targets: " + std::to_string(m_metrics.depthTargetMemory / 1024 / 1024) + " MB\n";
    result += "Active Targets: " + std::to_string(m_metrics.activeRenderTargets) + "\n";
    result += "Total Targets: " + std::to_string(m_metrics.totalRenderTargets) + "\n";

    return result;
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
#include <sstream>
#include <cmath>
#include <cstring>

// ============================================================================
// RenderTarget (Linux stub)
// ============================================================================

RenderTarget::RenderTarget(const RenderTargetDesc& desc) : m_desc(desc) {}
RenderTarget::~RenderTarget() {}

HRESULT RenderTarget::Create(ID3D11Device* /*device*/)
{
    return S_OK;
}

void RenderTarget::Destroy()
{
    // No GPU resources on Linux
}

HRESULT RenderTarget::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_desc.width = width;
    m_desc.height = height;
    Destroy();
    return Create(device);
}

void RenderTarget::Clear(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void RenderTarget::GenerateMips(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

std::string RenderTarget::GetInfo() const
{
    std::string info = "RenderTarget: " + m_desc.name + "\n";
    info += "Size: " + std::to_string(m_desc.width) + "x" + std::to_string(m_desc.height) + "\n";
    info += "Array Size: " + std::to_string(m_desc.arraySize) + "\n";
    info += "Mip Levels: " + std::to_string(m_desc.mipLevels) + "\n";
    info += "Sample Count: " + std::to_string(m_desc.sampleCount) + "\n";
    info += "Valid: " + std::string(IsValid() ? "Yes" : "No") + "\n";
    info += "Depth/Stencil: " + std::string(IsDepthStencil() ? "Yes" : "No") + "\n";
    info += "Multisampled: " + std::string(IsMultisampled() ? "Yes" : "No") + "\n";
    return info;
}

bool RenderTarget::SaveToFile(const std::string& /*filename*/) const
{
    return false; // No GPU readback on Linux
}

DXGI_FORMAT RenderTarget::GetDXGIFormat(RenderTargetFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}
DXGI_FORMAT RenderTarget::GetTypelessFormat(RenderTargetFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}
DXGI_FORMAT RenderTarget::GetSRVFormat(RenderTargetFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}
DXGI_FORMAT RenderTarget::GetDSVFormat(RenderTargetFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}

bool RenderTarget::SaveBMP(const std::string& /*filename*/, unsigned char* /*data*/, uint32_t /*width*/,
                           uint32_t /*height*/, uint32_t /*pitch*/) const
{
    return false;
}

// ============================================================================
// MultipleRenderTargets (Linux stub)
// ============================================================================

MultipleRenderTargets::MultipleRenderTargets(const std::string& name) : m_name(name) {}

MultipleRenderTargets::~MultipleRenderTargets() {}

void MultipleRenderTargets::AddRenderTarget(std::shared_ptr<RenderTarget> renderTarget, uint32_t slot)
{
    m_renderTargets[slot] = renderTarget;
}

void MultipleRenderTargets::SetDepthStencil(std::shared_ptr<RenderTarget> depthStencil)
{
    m_depthStencil = depthStencil;
}

HRESULT MultipleRenderTargets::Create(ID3D11Device* device)
{
    HRESULT hr = S_OK;
    for (auto& pair : m_renderTargets)
    {
        hr = pair.second->Create(device);
        if (FAILED(hr))
            return hr;
    }
    if (m_depthStencil)
    {
        hr = m_depthStencil->Create(device);
    }
    return hr;
}

void MultipleRenderTargets::Bind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void MultipleRenderTargets::Unbind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void MultipleRenderTargets::Clear(ID3D11DeviceContext* context)
{
    for (auto& pair : m_renderTargets)
    {
        pair.second->Clear(context);
    }
    if (m_depthStencil)
    {
        m_depthStencil->Clear(context);
    }
}

HRESULT MultipleRenderTargets::Resize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    HRESULT hr = S_OK;
    for (auto& pair : m_renderTargets)
    {
        hr = pair.second->Resize(device, width, height);
        if (FAILED(hr))
            return hr;
    }
    if (m_depthStencil)
    {
        hr = m_depthStencil->Resize(device, width, height);
    }
    return hr;
}

std::shared_ptr<RenderTarget> MultipleRenderTargets::GetRenderTarget(uint32_t slot) const
{
    auto it = m_renderTargets.find(slot);
    return (it != m_renderTargets.end()) ? it->second : nullptr;
}

// ============================================================================
// RenderTargetManager (Linux stub)
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

RenderTargetManager::RenderTargetMetrics RenderTargetManager::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string RenderTargetManager::Console_ListRenderTargets() const
{
    std::string result = "=== Render Targets ===\n";
    for (const auto& pair : m_renderTargets)
    {
        const auto& desc = pair.second->GetDesc();
        result += pair.first + " (" + std::to_string(desc.width) + "x" + std::to_string(desc.height) + ", " +
                  FormatToString(desc.format) + ")\n";
    }
    result += "\n=== MRT Groups ===\n";
    for (const auto& pair : m_mrtGroups)
    {
        result += pair.first + " (" + std::to_string(pair.second->GetRenderTargetCount()) + " targets)\n";
    }
    return result;
}

std::string RenderTargetManager::Console_GetRenderTargetInfo(const std::string& name) const
{
    auto rt = GetRenderTarget(name);
    if (rt)
        return rt->GetInfo();
    return "Render target '" + name + "' not found";
}

bool RenderTargetManager::Console_SaveRenderTarget(const std::string& name, const std::string& filename)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
        return false;
    std::string actualFilename = filename.empty() ? (name + ".bmp") : filename;
    return rt->SaveToFile(actualFilename);
}

bool RenderTargetManager::Console_CreateRenderTarget(const std::string& name, uint32_t width, uint32_t height,
                                                     const std::string& format)
{
    RenderTargetDesc desc;
    desc.name = name;
    desc.width = width;
    desc.height = height;
    desc.format = StringToFormat(format);
    auto rt = CreateRenderTarget(desc);
    return rt != nullptr;
}

bool RenderTargetManager::Console_ResizeRenderTarget(const std::string& name, uint32_t width, uint32_t height)
{
    auto rt = GetRenderTarget(name);
    if (!rt)
        return false;
    return SUCCEEDED(rt->Resize(m_device, width, height));
}

void RenderTargetManager::Console_ToggleVisualization(const std::string& name)
{
    auto it = m_visualizationState.find(name);
    if (it != m_visualizationState.end())
    {
        it->second = !it->second;
    }
    else
    {
        auto rt = GetRenderTarget(name);
        if (rt)
        {
            m_visualizationState[name] = true;
        }
    }
}

void RenderTargetManager::Console_SetClearColor(const std::string& name, float r, float g, float b, float a)
{
    auto rt = GetRenderTarget(name);
    if (rt)
    {
        auto& desc = const_cast<RenderTargetDesc&>(rt->GetDesc());
        desc.clearColor = {r, g, b, a};
    }
}

void RenderTargetManager::Console_GarbageCollect()
{
    for (auto it = m_renderTargets.begin(); it != m_renderTargets.end();)
    {
        if (it->second.use_count() == 1)
        {
            it->second->Destroy();
            it = m_renderTargets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    UpdateMetrics();
}

int RenderTargetManager::Console_ValidateRenderTargets()
{
    int errors = 0;
    for (const auto& pair : m_renderTargets)
    {
        if (!pair.second->IsValid())
        {
            errors++;
        }
    }
    return errors;
}

std::string RenderTargetManager::Console_GetMemoryInfo() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    std::string result = "=== Render Target Memory Usage ===\n";
    result += "Total Memory: " + std::to_string(m_metrics.totalMemoryUsage / 1024 / 1024) + " MB\n";
    result += "Color Targets: " + std::to_string(m_metrics.colorTargetMemory / 1024 / 1024) + " MB\n";
    result += "Depth Targets: " + std::to_string(m_metrics.depthTargetMemory / 1024 / 1024) + " MB\n";
    result += "Active Targets: " + std::to_string(m_metrics.activeRenderTargets) + "\n";
    result += "Total Targets: " + std::to_string(m_metrics.totalRenderTargets) + "\n";
    return result;
}

void RenderTargetManager::RenderDebugVisualization(ID3D11DeviceContext* /*context*/, uint32_t /*screenWidth*/,
                                                   uint32_t /*screenHeight*/)
{
    // No-op on Linux
}

bool RenderTargetManager::IsVisualizationEnabled(const std::string& name) const
{
    auto it = m_visualizationState.find(name);
    return it != m_visualizationState.end() && it->second;
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
