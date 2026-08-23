/**
 * @file GraphicsEngineWindowsDeviceLost.cpp
 * @brief Windows/D3D11 device-lost recovery for GraphicsEngine
 *
 * ReleaseAllDeviceResources / RecoverFromDeviceLost split out of
 * GraphicsEngineWindows.cpp (which keeps lifecycle: construction,
 * device-attach initialization, shutdown, and resize). Linux counterpart
 * lives in GraphicsEngineLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/SparkConsole.h"

#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// DEVICE LOST RECOVERY
// ============================================================================

void GraphicsEngine::ReleaseAllDeviceResources()
{
    // Shutdown subsystems that hold device references
    if (m_textureSystem)
        m_textureSystem->Shutdown();
    if (m_materialSystem)
        m_materialSystem->Shutdown();
    if (m_lightingSystem)
        m_lightingSystem->Shutdown();
    if (m_assetPipeline)
        m_assetPipeline->Shutdown();

    m_pipelineStateCache.Shutdown();
    m_renderTargetPool.Shutdown();
    m_gpuSceneBuffer.Shutdown();
    m_constantBufferRing.Shutdown();
    m_gpuDebugMarkers.Shutdown();
    m_gpuTimestampQuery.Shutdown();

    // Release render targets
    m_hdrSRV.Reset();
    m_hdrRTV.Reset();
    m_hdrTexture.Reset();
    for (auto& srv : m_gBufferSRVs)
        srv.Reset();
    for (auto& rtv : m_gBufferRTVs)
        rtv.Reset();
    for (auto& tex : m_gBufferTextures)
        tex.Reset();

    // Release render states and queries
    m_defaultBlendState.Reset();
    m_defaultDepthState.Reset();
    m_gpuTimingQuery.Reset();
    m_wireframeRasterState.Reset();
    m_solidRasterState.Reset();

    // Release core device resources
    m_depthStencilSRV.Reset();
    m_depthStencilView.Reset();
    m_depthStencilTexture.Reset();
    m_backBufferSRV.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();

    // Release basic shader resources
    m_basicVertexShader.Reset();
    m_basicPixelShader.Reset();
    m_basicInputLayout.Reset();
    m_basicConstantBuffer.Reset();
    m_basicFrameConstantBuffer.Reset();
    m_basicSamplerState.Reset();
    m_defaultTexture.Reset();
    m_defaultSRV.Reset();

    // Flush the context before releasing device
    if (m_context)
        m_context->ClearState();
    m_context.Reset();
    m_device.Reset();
}

bool GraphicsEngine::RecoverFromDeviceLost()
{
    m_deviceLostRecoveryAttempts++;
    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "DEVICE LOST RECOVERY: Attempt %u/%u — recreating D3D11 device",
                   m_deviceLostRecoveryAttempts, MAX_DEVICE_RECOVERY);

    ReleaseAllDeviceResources();

    HWND hwnd = static_cast<HWND>(m_hwnd);
    HRESULT hr = CreateDeviceAndSwapChain(hwnd);
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: CreateDeviceAndSwapChain failed (HR=0x%08lX). "
                        "Falling back to headless mode.",
                        static_cast<long>(hr));
        SPARK_DEBUG_HOOK_SYSTEM(DeviceLostFallback, "Graphics", 0.0);
        return false;
    }

    hr = CreateRenderTargetView();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: CreateRenderTargetView failed (HR=0x%08lX)", static_cast<long>(hr));
        return false;
    }

    hr = CreateDepthStencilView();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: CreateDepthStencilView failed (HR=0x%08lX)", static_cast<long>(hr));
        return false;
    }

    CreateAdvancedRenderTargets();
    CreateRenderStates();
    SetViewport();
    ApplyGraphicsState();

    // Re-initialize subsystems with new device
    if (m_textureSystem)
        m_textureSystem->Initialize(m_device.Get(), m_context.Get());
    if (m_materialSystem)
        m_materialSystem->Initialize(m_device.Get(), m_context.Get());
    if (m_lightingSystem)
        m_lightingSystem->Initialize(m_device.Get(), m_context.Get());
    if (m_assetPipeline)
        m_assetPipeline->Initialize(m_device.Get(), m_context.Get());

    m_deviceLostRecoveryAttempts = 0; // Reset on success
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DEVICE LOST RECOVERY: Successfully recreated D3D11 device");
    SPARK_DEBUG_HOOK_SYSTEM(DeviceRecovered, "Graphics", 0.0);
    return true;
}

#endif // SPARK_PLATFORM_WINDOWS
