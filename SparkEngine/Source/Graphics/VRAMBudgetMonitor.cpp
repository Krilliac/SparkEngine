/**
 * @file VRAMBudgetMonitor.cpp
 * @brief GPU memory budget monitoring implementation
 */

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "VRAMBudgetMonitor.h"
#include "../Utils/SparkConsole.h"
#include <dxgi1_4.h>

using Microsoft::WRL::ComPtr;

HRESULT VRAMBudgetMonitor::Initialize(ID3D11Device* device)
{
    if (m_initialized)
    {
        return S_FALSE;
    }
    if (!device)
    {
        return E_INVALIDARG;
    }

    // Walk the DXGI chain: Device → DXGIDevice → Adapter → Adapter3
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
    {
        return hr;
    }

    // Capture total dedicated VRAM from the adapter description
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (SUCCEEDED(hr))
    {
        m_totalVRAM = static_cast<size_t>(desc.DedicatedVideoMemory);
    }

    // IDXGIAdapter3 is available on Windows 10+ and gives live budget info
    hr = adapter->QueryInterface(IID_PPV_ARGS(&m_adapter3));
    m_querySupported = SUCCEEDED(hr);

    if (m_querySupported)
    {
        // Seed initial values
        Update();
    }
    else
    {
        // Fallback: use 75 % of total VRAM as a conservative budget
        m_budget = m_totalVRAM;
        m_recommendedTextureBudget = m_totalVRAM * 3 / 4;
    }

    m_initialized = true;
    return S_OK;
}

void VRAMBudgetMonitor::Shutdown()
{
    m_adapter3.Reset();
    m_initialized = false;
    m_querySupported = false;
}

void VRAMBudgetMonitor::Update()
{
    if (!m_querySupported || !m_adapter3)
    {
        return;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    HRESULT hr = m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (SUCCEEDED(hr))
    {
        m_currentUsage = static_cast<size_t>(info.CurrentUsage);
        m_budget = static_cast<size_t>(info.Budget);
    }

    ComputePressure();
}

void VRAMBudgetMonitor::ComputePressure()
{
    if (m_budget == 0)
    {
        m_pressureRatio = 0.0f;
        m_pressureLevel = VRAMPressureLevel::None;
        m_recommendedTextureBudget = 512 * 1024 * 1024;
        return;
    }

    m_pressureRatio = static_cast<float>(m_currentUsage) / static_cast<float>(m_budget);

    if (m_pressureRatio > 0.95f)
    {
        m_pressureLevel = VRAMPressureLevel::Critical;
    }
    else if (m_pressureRatio > 0.85f)
    {
        m_pressureLevel = VRAMPressureLevel::High;
    }
    else if (m_pressureRatio > 0.75f)
    {
        m_pressureLevel = VRAMPressureLevel::Medium;
    }
    else if (m_pressureRatio > 0.60f)
    {
        m_pressureLevel = VRAMPressureLevel::Low;
    }
    else
    {
        m_pressureLevel = VRAMPressureLevel::None;
    }

    // Reserve a portion of the OS budget for non-texture GPU resources
    // (render targets, buffers, shaders, etc.).  Textures get ~60 % of the
    // remaining headroom, scaled down under pressure.
    size_t headroom = (m_budget > m_currentUsage) ? (m_budget - m_currentUsage) : 0;
    size_t textureShare = headroom * 60 / 100;

    // Floor: never recommend less than 64 MB
    constexpr size_t kMinBudget = 64 * 1024 * 1024;
    m_recommendedTextureBudget = (textureShare > kMinBudget) ? textureShare : kMinBudget;
}

#else // !SPARK_PLATFORM_WINDOWS

#include "VRAMBudgetMonitor.h"

HRESULT VRAMBudgetMonitor::Initialize(ID3D11Device* /*device*/)
{
    m_initialized = true;
    // No DXGI on non-Windows; keep defaults
    return S_OK;
}

void VRAMBudgetMonitor::Shutdown()
{
    m_initialized = false;
}

void VRAMBudgetMonitor::Update()
{
    // No-op on non-Windows
}

void VRAMBudgetMonitor::ComputePressure()
{
    // No-op on non-Windows
}

#endif // SPARK_PLATFORM_WINDOWS
