/**
 * @file GraphicsEngineWindowsAccessors.cpp
 * @brief Windows/D3D11 system/state accessors for GraphicsEngine
 *
 * Subsystem getters and D3D11 accessors split out of
 * GraphicsEngineWindows.cpp (which keeps lifecycle: construction,
 * device-attach initialization, shutdown, and resize). Linux counterpart
 * lives in GraphicsEngineLinuxAccessors.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Phase Q: Denoiser accessor
// ============================================================================

Spark::Graphics::DenoiserBackend GraphicsEngine::GetDenoiserBackend() const
{
    return m_denoiser ? m_denoiser->GetBackend() : Spark::Graphics::DenoiserBackend::None;
}

// ============================================================================
// Phase S: Procedural noise accessor
// ============================================================================

Spark::Graphics::SIMDLevel GraphicsEngine::GetProceduralNoiseSIMDLevel() const
{
    return Spark::Graphics::DetectBestSIMD();
}

// ============================================================================
// SYSTEM ACCESSORS
// ============================================================================

ID3D11Device* GraphicsEngine::GetDevice() const
{
    return m_device.Get();
}

ID3D11DeviceContext* GraphicsEngine::GetContext() const
{
    return m_context.Get();
}

UINT GraphicsEngine::GetWindowWidth() const
{
    return m_windowWidth;
}

UINT GraphicsEngine::GetWindowHeight() const
{
    return m_windowHeight;
}

const RenderStatistics& GraphicsEngine::GetStatistics() const
{
    return m_statistics;
}

TextureSystem* GraphicsEngine::GetTextureSystem() const
{
    return m_textureSystem.get();
}

MaterialSystem* GraphicsEngine::GetMaterialSystem() const
{
    return m_materialSystem.get();
}

LightingSystem* GraphicsEngine::GetLightingSystem() const
{
    return m_lightingSystem.get();
}

Spark::Graphics::PostProcessingPipeline* GraphicsEngine::GetPostProcessingPipeline() const
{
    return m_postProcessing.get();
}

AssetPipeline* GraphicsEngine::GetAssetPipeline() const
{
    return m_assetPipeline.get();
}

Spark::RHI::RHIBridge* GraphicsEngine::GetRHIBridge() const
{
    // The Windows renderer drives D3D11 directly and owns no RHI bridge.
    return nullptr;
}

Spark::RHI::IRHIDevice* GraphicsEngine::GetRHIDevice() const
{
    return nullptr;
}

LightManager* GraphicsEngine::GetLightManager() const
{
    return m_lightManager.get();
}

RenderingPipeline GraphicsEngine::GetRenderingPipeline() const
{
    return m_currentPipeline;
}

const GraphicsSettings& GraphicsEngine::GetGraphicsSettings() const
{
    return m_settings;
}

IDXGISwapChain* GraphicsEngine::GetSwapChain() const
{
    return m_swapChain.Get();
}

ID3D11RenderTargetView* GraphicsEngine::GetBackBufferRTV() const
{
    return m_renderTargetView.Get();
}

ID3D11DepthStencilView* GraphicsEngine::GetDepthStencilView() const
{
    return m_depthStencilView.Get();
}


#endif // SPARK_PLATFORM_WINDOWS
