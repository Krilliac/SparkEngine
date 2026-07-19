/**
 * @file RenderTargetWindowsMulti.cpp
 * @brief Windows/D3D11 MultipleRenderTargets implementation — split from RenderTargetWindows.cpp
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "RenderTarget.h"

#ifdef SPARK_PLATFORM_WINDOWS

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


#endif // SPARK_PLATFORM_WINDOWS
