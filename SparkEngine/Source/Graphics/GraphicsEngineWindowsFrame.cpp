/**
 * @file GraphicsEngineWindowsFrame.cpp
 * @brief Windows/D3D11 per-frame rendering path for GraphicsEngine
 *
 * BeginFrame / EndFrame / RenderScene / AcquireHybridRTBindings split out of
 * GraphicsEngineWindows.cpp (which keeps lifecycle: construction,
 * device-attach initialization, shutdown, and resize). Linux counterpart
 * lives in GraphicsEngineLinuxFrame.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"
#include "../Core/FaultIsolation.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include "VRAMBudgetMonitor.h"
#include "FoliageRenderer.h"

#include "TemporalEffects.h"
#include "PostProcessingPipeline.h"
#include "ShadowAtlas.h"
using Spark::Graphics::PostProcessingPipeline;
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#ifdef SPARK_HARDWARE_RT
#include "RHI/DXRSupport.h"
#endif
#endif
#include "ShaderHotReload.h"
#include "GPUDrivenRenderer.h"
#include "../Game/GameObject.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// FRAME MANAGEMENT
// ============================================================================

void GraphicsEngine::BeginFrame()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    if (m_attachedMode)
    {
        // Attached to a caller-owned device (no swapchain/backbuffer). The
        // caller drives its own render target binding/clearing directly.
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "BeginFrame() called on an attach-mode GraphicsEngine — skipping");
        return;
    }
    bool expected = false;
    if (!m_frameInProgress.compare_exchange_strong(expected, true))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "BeginFrame called while frame already in progress — skipping");
        return;
    }

    m_frameStartTime = std::chrono::high_resolution_clock::now();

    // Reset per-frame state tracking for new frame
    m_pipelineStateCache.ResetBoundState();
    m_sortedDrawList.clear();
    m_constantBufferRing.BeginFrame(m_context.Get());
    m_gpuTimestampQuery.BeginFrame(m_context.Get());
    if (m_shadowAtlas)
        m_shadowAtlas->BeginFrame();

    // Update VRAM budget monitor (lightweight DXGI query)
    if (m_vramBudgetMonitor)
        m_vramBudgetMonitor->Update();

    // Shader hot-reload: check for modified .hlsl files each frame.
    // m_shader is never instantiated, so calling via HotReloadShaders()
    // would be dead code. Pump the singleton directly instead.
    Spark::Graphics::ShaderHotReload::GetInstance().Update(1.0f / 60.0f);

    ASSERT(m_context && m_renderTargetView && m_depthStencilView);

    if (!m_context || !m_renderTargetView || !m_depthStencilView)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid render targets in BeginFrame", L"ERROR");
        m_frameInProgress = false;
        return;
    }

    // Build HiZ mip chain from previous frame's depth (must run before clear)
    if (m_settings.gpuDrivenRendering && m_depthStencilSRV)
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.IsInitialized())
        {
            gpuRenderer.BeginFrame(m_depthStencilSRV.Get(), m_windowWidth, m_windowHeight);
        }
    }

    // Clear render targets
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), m_settings.clearColor);
    m_context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Bind render targets
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());

    m_renderStartTime = std::chrono::high_resolution_clock::now();

    // Apply graphics state
    ApplyGraphicsState();

    // Start GPU timing if enabled
    if (m_gpuTimingQuery && m_settings.enableGPUTiming)
    {
        m_context->Begin(m_gpuTimingQuery.Get());
    }
}

void GraphicsEngine::EndFrame()
// NOTE: Intentionally exceeds 50-line guideline — linear rendering pipeline dispatch
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    if (m_attachedMode)
    {
        // Attached to a caller-owned device (no swapchain to Present). The
        // caller is responsible for its own present/flush.
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "EndFrame() called on an attach-mode GraphicsEngine — skipping");
        return;
    }
    bool expected = true;
    if (!m_frameInProgress.compare_exchange_strong(expected, false))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "EndFrame called without matching BeginFrame — skipping");
        return;
    }

    auto renderEndTime = std::chrono::high_resolution_clock::now();

    // Flush GPU scene buffer and tick render target pool
    SPARK_GUARDED_UPDATE("Graphics:GPUFlush", "Graphics", {
        if (m_gpuSceneBuffer.IsInitialized())
        {
            // Push the latest foliage batch into the GPU scene buffer
            // before the flush. The renderer's CollectFromFoliageManager
            // step is wired into the per-frame lifecycle update; this
            // handoff is the last hop that gets foliage instances to
            // the GPU. We append after the existing static-mesh slots
            // by starting at the buffer's current write head.
            auto& foliage = Spark::Graphics::FoliageRenderer::GetInstance();
            uint32_t foliageStartSlot = 0;
            uint32_t foliageUploadedCount = 0;
            if (foliage.IsInitialized())
            {
                foliageStartSlot = m_gpuSceneBuffer.GetActiveCount();
                const uint32_t written = foliage.UploadToSceneBuffer(m_gpuSceneBuffer, foliageStartSlot);
                if (written != UINT32_MAX && written > 0)
                {
                    foliageUploadedCount = written;
                    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Foliage: uploaded %u instances to GPU scene buffer",
                                    written);
                }
            }

            m_gpuSceneBuffer.FlushToGPU(m_context.Get());

            // Phase E: after the flush, issue the foliage draw pass.
            // Mesh instances draw the cached species meshes; impostor
            // instances draw camera-aligned billboards sampling the
            // impostor atlas SRV. Skips cleanly when there is no batch.
            //
            // Phase G fix: pass `foliageUploadedCount` so RenderFoliagePass
            // clamps draw runs to the slots actually written this frame
            // (UploadToSceneBuffer can stop early on GPUSceneBuffer
            // overflow), and pass an accumulated wall-clock time so
            // ComputeWindSway in FoliageVS.hlsl animates frame-to-frame
            // instead of being frozen at time=0.
            if (foliageUploadedCount > 0 && m_device && m_context)
            {
                static const auto s_foliageTimeOrigin = std::chrono::high_resolution_clock::now();
                const float foliageWindTime =
                    std::chrono::duration<float>(m_frameStartTime - s_foliageTimeOrigin).count();

                foliage.RenderFoliagePass(m_device.Get(), m_context.Get(), m_frameViewMatrix, m_frameProjMatrix,
                                          m_frameCameraPos, foliageWindTime, m_gpuSceneBuffer, foliageStartSlot,
                                          foliageUploadedCount);
            }
        }
        m_constantBufferRing.EndFrame();
        m_gpuTimestampQuery.EndFrame(m_context.Get());
        m_renderTargetPool.Tick();
        if (m_shadowAtlas)
            m_shadowAtlas->EndFrame();
    });

    if (!m_swapChain)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid swap chain in EndFrame", L"ERROR");
        return;
    }

    // End GPU timing if enabled
    if (m_gpuTimingQuery && m_settings.enableGPUTiming)
    {
        m_context->End(m_gpuTimingQuery.Get());
    }

    // Exe-side overlay hook (game-mode ImGui): draws on top of the finished
    // frame, immediately before Present. Plain function pointer so this call
    // is safe even when EndFrame() executes from a module DLL's code copy.
    if (m_prePresentHook)
    {
        // Re-bind the backbuffer (no depth) — post passes may have retargeted.
        if (m_context && m_renderTargetView)
            m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
        m_prePresentHook(m_prePresentHookUser);
    }

    UINT syncInterval = m_settings.vsync ? 1 : 0;
    HRESULT hr = m_swapChain->Present(syncInterval, 0);

    if (FAILED(hr))
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "Graphics", 5, "SwapChain::Present failed with HR=0x%08lX",
                                static_cast<long>(hr));

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            if (hr == DXGI_ERROR_DEVICE_REMOVED)
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

            // Attempt automatic recovery
            if (m_deviceLostRecoveryAttempts < MAX_DEVICE_RECOVERY)
            {
                if (!RecoverFromDeviceLost())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "DEVICE LOST RECOVERY: Failed — engine will continue in degraded mode");
                }
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                "DEVICE LOST RECOVERY: Max attempts (%u) exhausted — "
                                "engine will continue without GPU rendering",
                                MAX_DEVICE_RECOVERY);
            }
        }
    }

    auto frameEndTime = std::chrono::high_resolution_clock::now();

    // Update performance metrics
    UpdateMetrics();

    // Calculate timing
    auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(frameEndTime - m_frameStartTime);
    auto renderTime = std::chrono::duration_cast<std::chrono::microseconds>(renderEndTime - m_renderStartTime);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.frameTime = frameTime.count() / 1000.0f;
        m_statistics.renderTime = renderTime.count() / 1000.0f;
    }
}

// ============================================================================
// HybridRT GBuffer binding — Windows wraps D3D11 textures as RHI handles
// ============================================================================
// The shared GraphicsEngine::DispatchHybridRTPass (in GraphicsEngineHybridRT.cpp)
// calls this; Linux/macOS has its own stub implementation that returns empty
// bindings (the RHI bridge does not yet expose GBuffer textures on those
// platforms).

Spark::Graphics::HybridRTBindings GraphicsEngine::AcquireHybridRTBindings()
{
    Spark::Graphics::HybridRTBindings bindings;
    if (!m_rhiBridge)
        return bindings;
    auto* device = m_rhiBridge->GetDevice();
    if (!device)
        return bindings;

    // Helper — wrap a D3D11 ComPtr texture into an RHI texture, stash the
    // wrapper in `bindings.owned` to extend its lifetime past this call,
    // and return the raw pointer for bindings.{normals,depth,albedo,lighting}.
    auto wrap = [&](void* nativeHandle, Spark::RHI::PixelFormat format, Spark::RHI::RHITextureUsage usage,
                    const char* name) -> Spark::RHI::IRHITexture*
    {
        if (!nativeHandle)
            return nullptr;
        Spark::RHI::RHITextureDesc desc;
        desc.width = m_width;
        desc.height = m_height;
        desc.format = format;
        desc.usage = usage;
        desc.debugName = name;
        auto wrapped = device->WrapNativeTexture(nativeHandle, desc);
        auto* raw = wrapped.get();
        if (wrapped)
            bindings.owned.push_back(std::move(wrapped));
        return raw;
    };

    // GBuffer layout: [0]=Albedo, [1]=Normal, [2]=Material, [3]=Motion
    bindings.normals = wrap(m_gBufferTextures[1].Get(), Spark::RHI::PixelFormat::R16G16B16A16_FLOAT,
                            Spark::RHI::RHITextureUsage::ShaderResource, "GBuffer_Normals_Wrapped");
    bindings.depth = wrap(m_depthStencilTexture.Get(), Spark::RHI::PixelFormat::D24_UNORM_S8_UINT,
                          Spark::RHI::RHITextureUsage::ShaderResource, "Depth_Wrapped");
    bindings.albedo = wrap(m_gBufferTextures[0].Get(), Spark::RHI::PixelFormat::R8G8B8A8_UNORM,
                           Spark::RHI::RHITextureUsage::ShaderResource, "GBuffer_Albedo_Wrapped");
    bindings.lighting = wrap(m_hdrTexture.Get(), Spark::RHI::PixelFormat::R16G16B16A16_FLOAT,
                             Spark::RHI::RHITextureUsage::ShaderResource | Spark::RHI::RHITextureUsage::UnorderedAccess,
                             "HDR_Lighting_Wrapped");
    return bindings;
}

// ============================================================================
// SCENE RENDERING
// ============================================================================

void GraphicsEngine::RenderScene(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                 const std::vector<GameObject*>& objects)
// NOTE: Intentionally exceeds 50-line guideline — linear rendering pipeline dispatch
{
    // Reuse persistent buffer to avoid per-frame allocation
    m_culledObjectsBuffer.clear();
    const std::vector<GameObject*>* visibleObjectsPtr = &objects;

    if (m_settings.frustumCulling)
    {
        m_culledObjectsBuffer.reserve(objects.size());
        CullObjects(objects, viewMatrix, projMatrix, m_culledObjectsBuffer);
        visibleObjectsPtr = &m_culledObjectsBuffer;
    }

    const auto& visibleObjects = *visibleObjectsPtr;

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = 0;
        m_statistics.triangles = 0;
        m_statistics.vertices = 0;
        m_statistics.totalObjects = static_cast<uint32_t>(objects.size());
        m_statistics.visibleObjects = static_cast<uint32_t>(visibleObjects.size());
    }

    // Feed temporal effects with frame matrices for TAA jitter and motion vectors
    if (m_temporalEffects)
    {
        XMFLOAT4X4 viewF, projF;
        XMStoreFloat4x4(&viewF, viewMatrix);
        XMStoreFloat4x4(&projF, projMatrix);
        float dt = m_statistics.frameTime / 1000.0f;
        m_temporalEffects->BeginFrame(viewF, projF, dt);
    }

    // Choose rendering path
    switch (m_currentPipeline)
    {
    case RenderingPipeline::Forward:
        RenderForward(viewMatrix, projMatrix, visibleObjects);
        break;
    case RenderingPipeline::Deferred:
        RenderDeferred(viewMatrix, projMatrix, visibleObjects);
        break;
    case RenderingPipeline::ForwardPlus:
        RenderForwardPlus(viewMatrix, projMatrix, visibleObjects);
        break;
    case RenderingPipeline::Clustered:
        RenderForwardPlus(viewMatrix, projMatrix, visibleObjects);
        break;
    case RenderingPipeline::RenderGraphBased:
        if (m_renderPipeline)
        {
            XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
            XMFLOAT3 cameraPos;
            XMStoreFloat3(&cameraPos, invView.r[3]);
            UpdateFrameConstants(viewMatrix, projMatrix, cameraPos);
            m_renderPipeline->ExecuteFrame(viewMatrix, projMatrix, cameraPos);
        }
        else
        {
            RenderForward(viewMatrix, projMatrix, visibleObjects);
        }
        break;
    default:
        RenderForward(viewMatrix, projMatrix, visibleObjects);
        break;
    }

    // Apply ray tracing effects (reflections, shadows, GI, AO)
#ifdef SPARK_HARDWARE_RT
    if (m_hybridRT)
    {
        XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
        XMFLOAT3 cameraPos;
        XMStoreFloat3(&cameraPos, invView.r[3]);

        auto& dxr = Spark::Graphics::DXRManager::GetInstance();
        if (dxr.IsAvailable())
        {
            XMMATRIX viewProj = viewMatrix * projMatrix;

            // Rebuild TLAS each frame for dynamic objects
            dxr.BuildTLAS(m_dxrInstances);

            // Dispatch enabled RT effects
            const auto& settings = dxr.GetSettings();
            auto features = static_cast<uint32_t>(settings.enabledFeatures);

            if (features & static_cast<uint32_t>(Spark::Graphics::RTFeature::Reflections))
            {
                dxr.TraceReflections(viewProj, cameraPos);
            }
            if (features & static_cast<uint32_t>(Spark::Graphics::RTFeature::Shadows))
            {
                // Use primary light direction for shadow rays
                XMFLOAT3 lightDir = {0.0f, -1.0f, 0.5f};
                dxr.TraceShadows(lightDir);
            }
            if (features & static_cast<uint32_t>(Spark::Graphics::RTFeature::AmbientOcclusion))
            {
                dxr.TraceAmbientOcclusion(viewProj, cameraPos);
            }
            if (features & static_cast<uint32_t>(Spark::Graphics::RTFeature::GlobalIllumination))
            {
                dxr.TraceGlobalIllumination(viewProj, cameraPos);
            }
        }
    }
#endif

    // Apply temporal effects (TAA resolve, motion blur)
    RenderTemporalEffects();

    // Apply post-processing effects
    RenderPostProcessing();

    // End temporal frame
    if (m_temporalEffects)
    {
        m_temporalEffects->EndFrame();
    }

    // Update final statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.culledObjects = m_statistics.totalObjects - m_statistics.visibleObjects;
    }
}

#endif // SPARK_PLATFORM_WINDOWS
