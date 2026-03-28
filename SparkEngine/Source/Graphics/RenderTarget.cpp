#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file RenderTarget.cpp
 * @brief Core RenderTarget and MultipleRenderTargets implementation (D3D11)
 *
 * Contains the individual render target wrapper (Create, Destroy, Resize, Clear,
 * format conversion, file save) and the MRT group that binds multiple targets
 * simultaneously for deferred rendering passes.
 *
 * @author Spark Engine Team
 * @date 2025
 */

#include "RenderTarget.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/ScopeGuard.h"

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
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, m_desc.width > 0 && m_desc.height > 0,
                      "RenderTarget dimensions must be positive");

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
    auto closeFile = Spark::MakeScopeExit([&] { fclose(file); });

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

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "RenderTarget.h"
#include "../Utils/Validate.h"
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

#endif // SPARK_PLATFORM_WINDOWS
