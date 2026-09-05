/**
 * @file GraphicsEngineLinuxAccessors.cpp
 * @brief Linux RHI-bridge system/state accessors for GraphicsEngine
 *
 * Subsystem getters and D3D11-stub accessors split out of
 * GraphicsEngineLinux.cpp (which keeps lifecycle: Initialize / Shutdown /
 * Resize). Windows counterpart lives in GraphicsEngineWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHI.h"

using namespace Spark::Graphics::Detail;

// ============================================================================
// Phase Q: Denoiser accessor (Linux stub path)
// ============================================================================

Spark::Graphics::DenoiserBackend GraphicsEngine::GetDenoiserBackend() const
{
    return m_denoiser ? m_denoiser->GetBackend() : Spark::Graphics::DenoiserBackend::None;
}

// ============================================================================
// Phase S: Procedural noise accessor (Linux stub path)
// ============================================================================

Spark::Graphics::SIMDLevel GraphicsEngine::GetProceduralNoiseSIMDLevel() const
{
    return Spark::Graphics::DetectBestSIMD();
}

// ============================================================================
// System Accessors
// ============================================================================

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

Spark::RHI::RHIBridge* GraphicsEngine::GetRHIBridge() const
{
    auto& rhi = GetRHI();
    return rhi.initialized ? &rhi.bridge : nullptr;
}

Spark::RHI::IRHIDevice* GraphicsEngine::GetRHIDevice() const
{
    auto& rhi = GetRHI();
    return rhi.initialized ? rhi.bridge.GetDevice() : nullptr;
}

ID3D11Device* GraphicsEngine::GetDevice() const
{
    return nullptr;
}
ID3D11DeviceContext* GraphicsEngine::GetContext() const
{
    return nullptr;
}
UINT GraphicsEngine::GetWindowWidth() const
{
    return m_windowWidth;
}
UINT GraphicsEngine::GetWindowHeight() const
{
    return m_windowHeight;
}
IDXGISwapChain* GraphicsEngine::GetSwapChain() const
{
    return nullptr;
}
ID3D11RenderTargetView* GraphicsEngine::GetBackBufferRTV() const
{
    return nullptr;
}
ID3D11DepthStencilView* GraphicsEngine::GetDepthStencilView() const
{
    return nullptr;
}

#endif // !SPARK_PLATFORM_WINDOWS
