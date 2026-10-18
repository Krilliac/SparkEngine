/**
 * @file GraphicsEngineWindowsDeviceLost.cpp
 * @brief Windows/D3D11 device health for GraphicsEngine: device-lost recovery
 *        and debug-layer validation counting
 *
 * HandleDeviceLost / ReleaseAllDeviceResources / RecoverFromDeviceLost and the
 * RHI-210 validation counters, split out of GraphicsEngineWindows.cpp (which
 * keeps lifecycle: construction, device-attach initialization, shutdown, and
 * resize). Linux counterpart lives in GraphicsEngineLinux.cpp.
 *
 * Thread affinity: game thread (EndFrame and console commands run there).
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
#include "UpscalingSystem.h"
#include "VRAMBudgetMonitor.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "ShadowAtlas.h"
#include "ScreenSpaceEffects.h"
#include "GPUDrivenRenderer.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <optional>
#include <vector>

using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// DEBUG-LAYER VALIDATION COUNTERS (RHI-210)
// ============================================================================

void GraphicsEngine::AccumulateValidationMessages(ID3D11InfoQueue* queue, ValidationCounts& counts)
{
    if (!queue)
        return;

    // Only runs while the debug layer is on, which is a diagnostic mode, so the
    // message buffer may allocate.
    constexpr uint64_t kLoggedFindingLimit = 32;
    std::vector<uint8_t> storage;
    const UINT64 stored = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < stored; ++index)
    {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &length)) || length < sizeof(D3D11_MESSAGE))
            continue;
        storage.resize(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(index, message, &length)))
            continue;

        const char* severity = nullptr;
        switch (message->Severity)
        {
        case D3D11_MESSAGE_SEVERITY_CORRUPTION:
            ++counts.corruption;
            severity = "CORRUPTION";
            break;
        case D3D11_MESSAGE_SEVERITY_ERROR:
            ++counts.errors;
            severity = "ERROR";
            break;
        case D3D11_MESSAGE_SEVERITY_WARNING:
            ++counts.warnings;
            break;
        default:
            break;
        }
        if (severity && counts.corruption + counts.errors <= kLoggedFindingLimit)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "D3D11 validation %s (id %d): %s", severity,
                            static_cast<int>(message->ID), message->pDescription ? message->pDescription : "");
        }
    }
    queue->ClearStoredMessages();
}

std::optional<GraphicsEngine::ValidationCounts> GraphicsEngine::GetValidationCounts()
{
    if (!m_infoQueue)
        return std::nullopt;
    AccumulateValidationMessages(m_infoQueue.Get(), m_validationCounts);
    return m_validationCounts;
}

// ============================================================================
// DEVICE LOST RECOVERY
// ============================================================================

bool GraphicsEngine::HandleDeviceLost(HRESULT presentResult)
{
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
    {
        HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
        SPARK_LOG_FATAL("Graphics",
                        "GPU DEVICE REMOVED -- Reason HR=0x%08lX. "
                        "Possible causes: driver crash, GPU hang, TDR timeout, or hardware fault",
                        static_cast<long>(reason));
    }
    else
    {
        SPARK_LOG_FATAL("Graphics", "GPU DEVICE RESET -- The GPU device was reset. "
                                    "This may indicate a driver update or GPU resource exhaustion");
    }

    if (m_deviceLostRecoveryAttempts >= MAX_DEVICE_RECOVERY)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: Max attempts (%u) exhausted — "
                        "engine will continue without GPU rendering",
                        MAX_DEVICE_RECOVERY);
        return false;
    }
    if (!RecoverFromDeviceLost())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: Failed — engine will continue in degraded mode");
        return false;
    }
    return true;
}

void GraphicsEngine::ReleaseAllDeviceResources()
{
    // Count what the lost device's debug layer recorded before it goes away.
    AccumulateValidationMessages(m_infoQueue.Get(), m_validationCounts);
    m_infoQueue.Reset();

    // Shut down every subsystem CreateDeviceDependentResources() initializes
    // again. One left running keeps, and later binds, objects owned by the lost
    // device (UpscalingSystem and VRAMBudgetMonitor even refuse to re-initialize
    // while they still think they are live).
    if (m_textureSystem)
        m_textureSystem->Shutdown();
    if (m_materialSystem)
        m_materialSystem->Shutdown();
    if (m_lightingSystem)
        m_lightingSystem->Shutdown();
    if (m_assetPipeline)
        m_assetPipeline->Shutdown();
    if (m_upscalingSystem)
        m_upscalingSystem->Shutdown();
    if (m_vramBudgetMonitor)
        m_vramBudgetMonitor->Shutdown();
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.IsInitialized())
            gpuRenderer.Shutdown();
    }
    if (m_postProcessing)
        m_postProcessing->Shutdown();
    if (m_temporalEffects)
        m_temporalEffects->Shutdown();
    if (m_shadowAtlas)
        m_shadowAtlas->Shutdown();
    if (m_screenSpaceEffects)
        m_screenSpaceEffects->Shutdown();
    if (m_terrainRenderer)
        m_terrainRenderer->Shutdown();

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
    m_defaultRasterState.Reset();
    m_gpuTimingQuery.Reset();
    m_disjointQuery.Reset();
    m_timestampStartQuery.Reset();
    m_timestampEndQuery.Reset();
    m_timestampBegin.Reset();
    m_timestampEnd.Reset();
    m_timestampDisjoint.Reset();
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

    // Lazily created basic-path objects and caches. Each is recreated on first
    // use after recovery; keeping them would bind the lost device's objects to
    // the new device's context.
    m_defaultNormalSRV.Reset();
    m_defaultNormalTexture.Reset();
    m_defaultRoughnessSRV.Reset();
    m_defaultRoughnessTexture.Reset();
    m_blobShadowSRV.Reset();
    m_blobShadowTexture.Reset();
    m_blendOpaque.Reset();
    m_blendAlpha.Reset();
    m_blendAdditive.Reset();
    m_depthReadOnly.Reset();
    m_basicTextureCache.clear();
    m_failedBasicTexturePaths.clear();
    m_basicMaterialCache.clear();
    m_basicMaterialAliases.clear();
    m_failedBasicMaterialPaths.clear();
    m_basicVertexShaderInstanced.Reset();
    m_basicInputLayoutInstanced.Reset();
    m_basicInstanceBuffer.Reset();
    m_basicInstanceCapacity = 0;
    m_basicInstancedTried = false;

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

    // Recovery needs the window the swap chain was created for. Bail out before
    // tearing anything down rather than releasing every resource and then failing
    // CreateSwapChain with a null OutputWindow.
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: no window handle (device-attach mode) — cannot recreate the "
                        "swap chain");
        SPARK_DEBUG_HOOK_SYSTEM(DeviceLostFallback, "Graphics", 0.0);
        return false;
    }

    ReleaseAllDeviceResources();

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

    // Recreate every device-dependent resource ReleaseAllDeviceResources() tore
    // down — render targets and states, the device-backed subsystems, the
    // renderer-integration caches and the basic shader pipeline. Re-running only
    // a handful of them used to leave the renderer with null shaders and an
    // uninitialized constant-buffer ring while reporting a successful recovery.
    hr = CreateDeviceDependentResources();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                        "DEVICE LOST RECOVERY: CreateDeviceDependentResources failed (HR=0x%08lX)",
                        static_cast<long>(hr));
        return false;
    }

    m_deviceLostRecoveryAttempts = 0; // Reset on success
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DEVICE LOST RECOVERY: Successfully recreated D3D11 device");
    SPARK_DEBUG_HOOK_SYSTEM(DeviceRecovered, "Graphics", 0.0);
    return true;
}

#endif // SPARK_PLATFORM_WINDOWS
