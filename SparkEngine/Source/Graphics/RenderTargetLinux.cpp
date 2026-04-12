/**
 * @file RenderTargetLinux.cpp
 * @brief Linux implementation — split from RenderTarget.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


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


#endif // !SPARK_PLATFORM_WINDOWS
