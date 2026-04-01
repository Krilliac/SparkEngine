#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file GraphicsEngine.cpp
 * @brief Central rendering orchestrator for SparkEngine (D3D11)
 *
 * Core lifecycle: construction, initialization, shutdown, frame management,
 * scene rendering dispatch, ECS draw submission, and system accessors.
 *
 * Rendering pipeline implementations are in GraphicsRenderPipelines.cpp.
 * Device/resource creation and shaders are in GraphicsDeviceResources.cpp.
 * Console integration methods are in GraphicsConsoleOps.cpp.
 * State management, metrics, and settings are in GraphicsStateAndSettings.cpp.
 */
#include "GraphicsEngine.h"
#include "../Core/FaultIsolation.h"
#include "../Utils/Assert.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/SparkError.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"

#include "TemporalEffects.h"
#include "LightManager.h"
#include "PostProcessingPipeline.h"
#include "ShadowAtlas.h"
#include "ScreenSpaceEffects.h"
#include "TerrainRenderer.h"
using Spark::Graphics::PostProcessingPipeline;
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#ifdef SPARK_HARDWARE_RT
#include "RHI/DXRSupport.h"
#endif
#endif
#include "Shader.h"
#include "RenderTarget.h"
#include "../Physics/PhysicsSystem.h"
#include "../Game/GameObject.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <d3dcompiler.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <string>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <thread>
#include <cmath>
#include <ctime>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>
#include <cfloat>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// ============================================================================

GraphicsEngine::GraphicsEngine()
    : m_windowWidth(1280), m_windowHeight(720), m_width(1280), m_height(720), m_fullscreen(false), m_hwnd(nullptr),
      m_textureMemoryUsage(0), m_bufferMemoryUsage(0), m_frameInProgress(false),
      m_currentPipeline(RenderingPipeline::Forward), m_hdrEnabled(false), m_msaaLevel(MSAALevel::None),
      m_physicsSystem()
{
    // Initialize unified settings to defaults
    m_settings.vsync = true;
    m_settings.wireframeMode = false;
    m_settings.debugMode = false;
    m_settings.showFPS = false;
    m_settings.clearColor[0] = 0.0f;
    m_settings.clearColor[1] = 0.2f;
    m_settings.clearColor[2] = 0.4f;
    m_settings.clearColor[3] = 1.0f;
    m_settings.renderScale = 1.0f;
    m_settings.hdr = false;
    m_settings.msaaSamples = 1;
    m_settings.enableGPUTiming = false;
    m_settings.frustumCulling = true;
    m_settings.shadows = true;
    m_settings.bloom = true;
    m_settings.ssao = false;
    m_settings.taa = false;
    m_settings.shadowMapSize = 2048;
    m_settings.maxTextureSize = 2048;
    m_settings.anisotropyLevel = 8;

    // Initialize advanced settings
    m_taaSettings = TAASettings();
    m_ssaoSettings = SSAOSettings();
    m_ssrSettings = SSRSettings();
    m_volumetricSettings = VolumetricSettings();

    // Initialize statistics
    m_statistics = {};

    // Create advanced systems
    try
    {
        m_textureSystem = std::make_unique<TextureSystem>();
        m_materialSystem = std::make_unique<MaterialSystem>();
        m_lightingSystem = std::make_unique<LightingSystem>();
        m_assetPipeline = std::make_unique<AssetPipeline>();

        m_lightManager = std::make_unique<LightManager>();
        m_renderPipeline = std::make_unique<Spark::Graphics::RenderPipeline>();
        m_renderPipeline->SetGraphicsEngine(this);
        m_postProcessing = std::make_unique<PostProcessingPipeline>();
        m_temporalEffects = std::make_unique<TemporalEffects>();
        m_shadowAtlas = std::make_unique<Spark::Graphics::ShadowAtlas>();
        m_screenSpaceEffects = std::make_unique<Spark::Graphics::ScreenSpaceEffects>();
        m_terrainRenderer = std::make_unique<Spark::Graphics::TerrainRenderer>();

        LOG_TO_CONSOLE_IMMEDIATE(L"Advanced systems created successfully", L"INFO");
    }
    catch (const std::exception&)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Some advanced systems failed to create", L"WARNING");
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine constructed with unified architecture and atomic frame management.",
                             L"INFO");
}

GraphicsEngine::~GraphicsEngine()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine destructor called.", L"INFO");
    Shutdown();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

HRESULT GraphicsEngine::Initialize(Spark::NativeWindowHandle hWnd)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Graphics", 0.0);
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::Initialize started with critical fixes.", L"INFO");
    ASSERT(hWnd != nullptr);
    if (!hWnd)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: hWnd is null in GraphicsEngine::Initialize", L"ERROR");
        return E_INVALIDARG;
    }

    HWND hwnd = static_cast<HWND>(hWnd);
    RECT rc;
    if (hwnd)
    {
        GetClientRect(hwnd, &rc);
    }
    else
    {
        rc.left = rc.top = rc.right = rc.bottom = 0;
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid window handle in GraphicsEngine::Initialize", L"ERROR");
    }
    m_windowWidth = rc.right - rc.left;
    m_windowHeight = rc.bottom - rc.top;

    std::wstring sizeMsg = L"Window size: " + std::to_wstring(m_windowWidth) + L"x" + std::to_wstring(m_windowHeight);
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"INFO");
    ASSERT_MSG(m_windowWidth > 0 && m_windowHeight > 0, "Invalid window size");

    HRESULT hr = CreateDeviceAndSwapChain(hwnd);
    ASSERT_MSG(SUCCEEDED(hr), "CreateDeviceAndSwapChain failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateDeviceAndSwapChain failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateRenderTargetView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateRenderTargetView failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateRenderTargetView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateDepthStencilView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateDepthStencilView failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateDepthStencilView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create rasterizer states
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_BACK;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthBias = 0;
    rastDesc.DepthBiasClamp = 0.0f;
    rastDesc.SlopeScaledDepthBias = 0.0f;
    rastDesc.DepthClipEnable = TRUE;
    rastDesc.ScissorEnable = FALSE;
    rastDesc.MultisampleEnable = FALSE;
    rastDesc.AntialiasedLineEnable = FALSE;

    hr = m_device->CreateRasterizerState(&rastDesc, &m_solidRasterState);
    ASSERT_MSG(SUCCEEDED(hr), "CreateRasterizerState (solid) failed");
    if (FAILED(hr))
        return hr;

    rastDesc.FillMode = D3D11_FILL_WIREFRAME;
    hr = m_device->CreateRasterizerState(&rastDesc, &m_wireframeRasterState);
    ASSERT_MSG(SUCCEEDED(hr), "CreateRasterizerState (wireframe) failed");
    if (FAILED(hr))
        return hr;

    // Create GPU timing query if supported
    if (m_settings.enableGPUTiming)
    {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&queryDesc, &m_gpuTimingQuery);
    }

    // Create advanced render targets and states
    hr = CreateAdvancedRenderTargets();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create advanced render targets", L"WARNING");
    }

    hr = CreateRenderStates();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create render states", L"WARNING");
    }

    SetViewport();
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();

    // Initialize advanced systems
    if (m_textureSystem)
    {
        hr = m_textureSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize TextureSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TextureSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_materialSystem)
    {
        hr = m_materialSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize MaterialSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"MaterialSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_lightingSystem)
    {
        hr = m_lightingSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize LightingSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"LightingSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_assetPipeline)
    {
        hr = m_assetPipeline->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize AssetPipeline", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"AssetPipeline initialized successfully", L"SUCCESS");
        }
    }

    if (m_physicsSystem)
    {
        hr = m_physicsSystem->Initialize();
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize PhysicsSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PhysicsSystem initialized successfully", L"SUCCESS");
        }
    }

    // Initialize renderer integration systems
    m_pipelineStateCache.Initialize(m_device.Get());
    m_renderTargetPool.Initialize(m_device.Get());
    m_gpuSceneBuffer.Initialize(m_device.Get(), 4096);
    m_constantBufferRing.Initialize(m_device.Get());
    m_gpuDebugMarkers.Initialize(m_context.Get());
    m_gpuTimestampQuery.Initialize(m_device.Get());

    // Initialize hybrid ray tracing system (SDFGI software fallback or hardware DXR)
#ifdef SPARK_HYBRID_RT
    if (m_rhiBridge)
    {
        m_hybridRT = std::make_unique<Spark::Graphics::HybridRTManager>();
        if (!m_hybridRT->Initialize(m_rhiBridge->GetDevice(), m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT: Falling back to screen-space only", L"WARNING");
            m_hybridRT.reset();
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT initialized successfully", L"SUCCESS");
#ifdef SPARK_HARDWARE_RT
            if (m_hybridRT->GetActiveBackend() == Spark::RHI::RayTracingBackend::HardwareDXR)
            {
                auto& dxr = Spark::Graphics::DXRManager::GetInstance();
                if (!dxr.IsAvailable())
                {
                    LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT: DXR hardware backend selected", L"INFO");
                }
            }
#endif
        }
    }
#endif

    // Initialize post-processing pipeline with D3D11 device
    if (m_postProcessing)
    {
        m_postProcessing->SetDevice(m_device.Get(), m_context.Get());
        if (m_postProcessing->Initialize(m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline GPU init failed (CPU-only mode)", L"WARNING");
        }
    }

    // Initialize temporal effects (TAA, motion blur) with D3D11 device
    if (m_temporalEffects)
    {
        m_temporalEffects->SetDevice(m_device.Get(), m_context.Get());
        m_temporalEffects->Initialize(m_windowWidth, m_windowHeight);
        if (m_temporalEffects->InitializeGPU())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects GPU init failed (CPU-only mode)", L"WARNING");
        }
    }

    // Initialize shadow atlas for shadow map management
    if (m_shadowAtlas)
    {
        uint32_t shadowAtlasSize = m_settings.shadowMapSize * 2;
        if (m_shadowAtlas->Initialize(shadowAtlasSize))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"ShadowAtlas initialized successfully", L"SUCCESS");
        }
    }

    // Initialize screen-space effects (SSAO, SSR, contact shadows)
    if (m_screenSpaceEffects)
    {
        if (m_screenSpaceEffects->Initialize(m_windowWidth, m_windowHeight))
        {
            m_screenSpaceEffects->SetSSAOEnabled(m_settings.ssao);
            LOG_TO_CONSOLE_IMMEDIATE(L"ScreenSpaceEffects initialized successfully", L"SUCCESS");
        }
    }

    // Initialize terrain renderer
    if (m_terrainRenderer)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_terrainRenderer->Initialize(m_device.Get()))
#else
        if (m_terrainRenderer->Initialize())
#endif
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TerrainRenderer initialized", L"SUCCESS");
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine initialization complete - rendering ready.", L"SUCCESS");

    // Initialize basic shaders for rendering
    HRESULT shaderResult = InitializeBasicShaders();
    if (FAILED(shaderResult))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to initialize basic shaders", L"WARNING");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Basic shaders initialized successfully", L"SUCCESS");
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Graphics", 0.0);
    return S_OK;
}

// ============================================================================
// SHUTDOWN
// ============================================================================

void GraphicsEngine::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Graphics", 0.0);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GraphicsEngine::Shutdown — beginning graphics subsystem teardown");
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::Shutdown called.", L"INFO");

    // Shutdown advanced systems
    if (m_textureSystem)
    {
        m_textureSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"TextureSystem shutdown complete", L"INFO");
    }

    if (m_materialSystem)
    {
        m_materialSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"MaterialSystem shutdown complete", L"INFO");
    }

    if (m_lightingSystem)
    {
        m_lightingSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"LightingSystem shutdown complete", L"INFO");
    }

    if (m_assetPipeline)
    {
        m_assetPipeline->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"AssetPipeline shutdown complete", L"INFO");
    }

    // PhysicsSystem lifecycle is now managed by SparkEngine.cpp / EngineContext
    m_physicsSystem = nullptr;

    // Shutdown hybrid ray tracing
#ifdef SPARK_HYBRID_RT
    if (m_hybridRT)
    {
        m_hybridRT->Shutdown();
        m_hybridRT.reset();
    }
#endif

    // Shutdown renderer integration systems
    m_pipelineStateCache.Shutdown();
    m_renderTargetPool.Shutdown();
    m_gpuSceneBuffer.Shutdown();
    m_constantBufferRing.Shutdown();
    m_gpuDebugMarkers.Shutdown();
    m_gpuTimestampQuery.Shutdown();

    // Shutdown legacy systems
    if (m_renderPipeline)
    {
        m_renderPipeline->Shutdown();
    }
    if (m_lightManager)
    {
        m_lightManager->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"LightManager shutdown complete", L"INFO");
    }

    if (m_postProcessing)
    {
        m_postProcessing->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline shutdown complete", L"INFO");
    }

    if (m_temporalEffects)
    {
        m_temporalEffects->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects shutdown complete", L"INFO");
    }

    if (m_shadowAtlas)
    {
        m_shadowAtlas->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"ShadowAtlas shutdown complete", L"INFO");
    }

    if (m_screenSpaceEffects)
    {
        m_screenSpaceEffects->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"ScreenSpaceEffects shutdown complete", L"INFO");
    }

    // Release DirectX resources
    m_hdrSRV.Reset();
    m_hdrRTV.Reset();
    m_hdrTexture.Reset();

    for (auto& srv : m_gBufferSRVs)
        srv.Reset();
    for (auto& rtv : m_gBufferRTVs)
        rtv.Reset();
    for (auto& tex : m_gBufferTextures)
        tex.Reset();

    m_defaultBlendState.Reset();
    m_defaultDepthState.Reset();
    m_gpuTimingQuery.Reset();
    m_wireframeRasterState.Reset();
    m_solidRasterState.Reset();
    m_depthStencilView.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine shutdown complete.", L"INFO");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Graphics", 0.0);
}

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
    m_depthStencilView.Reset();
    m_depthStencilTexture.Reset();
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

// ============================================================================
// FRAME MANAGEMENT
// ============================================================================

void GraphicsEngine::BeginFrame()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
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

    // Shader hot-reload: check for modified .hlsl files each frame
    if (m_shader)
        m_shader->HotReloadShaders();

    ASSERT(m_context && m_renderTargetView && m_depthStencilView);

    if (!m_context || !m_renderTargetView || !m_depthStencilView)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid render targets in BeginFrame", L"ERROR");
        m_frameInProgress = false;
        return;
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
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
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
            m_gpuSceneBuffer.FlushToGPU(m_context.Get());
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
// ECS MESH DRAW SUBMISSION
// ============================================================================

void GraphicsEngine::SubmitMeshForRendering(std::string_view meshPath, std::string_view materialPath,
                                            const DirectX::XMMATRIX& worldMatrix, bool castShadows)
{
    SPARK_WARN_IF(Spark::LogCategory::Graphics, meshPath.empty(), "SubmitMeshForRendering: empty meshPath");
    SPARK_WARN_IF(Spark::LogCategory::Graphics, materialPath.empty(), "SubmitMeshForRendering: empty materialPath");
    MeshDrawCommand cmd;
    cmd.meshPath = meshPath;
    cmd.materialPath = materialPath;
    XMStoreFloat4x4(&cmd.worldMatrix, worldMatrix);
    cmd.castShadows = castShadows;

    // Spinlock: lower overhead than std::mutex for short critical sections.
    // Draw submission is called per-entity but holds the lock only for a push_back.
    while (m_drawListSpinlock.test_and_set(std::memory_order_acquire))
    {
    }
    m_drawList.push_back(cmd);
    m_drawListSpinlock.clear(std::memory_order_release);
}

void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix)
{
    // Swap the draw list out under the spinlock — minimal hold time.
    std::vector<MeshDrawCommand> localDrawList;
    while (m_drawListSpinlock.test_and_set(std::memory_order_acquire))
    {
    }
    localDrawList = std::move(m_drawList);
    m_drawList.clear();
    // Pre-reserve capacity for next frame based on this frame's count
    if (m_drawList.capacity() == 0 && !localDrawList.empty())
    {
        m_drawList.reserve(localDrawList.size());
    }
    m_drawListSpinlock.clear(std::memory_order_release);

    if (localDrawList.empty())
        return;

    // Set up shaders for ECS mesh rendering
    SetBasicShaders();

    for (const auto& cmd : localDrawList)
    {
        XMMATRIX world = XMLoadFloat4x4(&cmd.worldMatrix);

        // Update per-object constant buffer with world/view/proj matrices
        UpdateBasicConstants(world, viewMatrix, projMatrix);

        // Bind mesh and material through the asset pipeline, then draw
        if (m_assetPipeline)
        {
            m_assetPipeline->BindMesh(std::string(cmd.meshPath));
            m_assetPipeline->BindMaterial(std::string(cmd.materialPath));
            m_assetPipeline->DrawBoundMesh();
        }

        m_statistics.drawCalls++;
    }
}

// ============================================================================
// SCENE RENDERING
// ============================================================================

void GraphicsEngine::RenderScene(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                 const std::vector<GameObject*>& objects)
{
    std::vector<GameObject*> culledObjects;
    const std::vector<GameObject*>* visibleObjectsPtr = &objects;

    if (m_settings.frustumCulling)
    {
        culledObjects.reserve(objects.size());
        CullObjects(objects, viewMatrix, projMatrix, culledObjects);
        visibleObjectsPtr = &culledObjects;
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
            // Rebuild TLAS each frame for dynamic objects
            dxr.BuildTLAS();

            // Dispatch enabled RT effects
            const auto& settings = dxr.GetSettings();
            auto effects = static_cast<uint32_t>(settings.enabledEffects);

            if (effects & static_cast<uint32_t>(Spark::RHI::RTEffect::Reflections))
            {
                dxr.TraceReflections(viewMatrix, projMatrix);
            }
            if (effects & static_cast<uint32_t>(Spark::RHI::RTEffect::Shadows))
            {
                dxr.TraceShadows(viewMatrix, projMatrix);
            }
            if (effects & static_cast<uint32_t>(Spark::RHI::RTEffect::AmbientOcclusion))
            {
                dxr.TraceAmbientOcclusion(viewMatrix, projMatrix);
            }
            if (effects & static_cast<uint32_t>(Spark::RHI::RTEffect::GlobalIllumination))
            {
                dxr.TraceGlobalIllumination(viewMatrix, projMatrix);
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

// ============================================================================
// RESIZE
// ============================================================================

HRESULT GraphicsEngine::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return E_INVALIDARG;

    OnResize(width, height);
    return S_OK;
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

Spark::RHI::IRHIDevice* GraphicsEngine::GetRHIDevice() const
{
    return m_rhiBridge ? m_rhiBridge->GetDevice() : nullptr;
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

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux implementation — routes rendering through the RHI bridge
// ============================================================================
#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "LightManager.h"
#include "PostProcessingPipeline.h"
using Spark::Graphics::PostProcessingPipeline;
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#endif
#include "RenderTarget.h"
#include "TemporalEffects.h"
#include "ScreenSpaceEffects.h"
#include "ShadowAtlas.h"
#include "../Physics/PhysicsSystem.h"
#include "../Game/GameObject.h"
#include "RHI/RHI.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <chrono>
#include <cmath>

using namespace Spark::Graphics::Detail;

// ============================================================================
// Construction / Destruction
// ============================================================================

GraphicsEngine::GraphicsEngine()
    : m_currentPipeline(RenderPath::Forward), m_settings(), m_statistics(), m_width(0), m_height(0),
      m_fullscreen(false), m_hwnd(nullptr), m_hdrEnabled(false), m_msaaLevel(MSAALevel::None), m_windowWidth(0),
      m_windowHeight(0), m_frameInProgress(false), m_textureMemoryUsage(0), m_bufferMemoryUsage(0)
{
}

GraphicsEngine::~GraphicsEngine()
{
    Shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

HRESULT GraphicsEngine::Initialize(Spark::NativeWindowHandle hWnd)
{
    m_hwnd = hWnd;

    m_width = 1280;
    m_height = 720;
    m_windowWidth = m_width;
    m_windowHeight = m_height;

    auto& rhi = GetRHI();

    Spark::RHI::GraphicsBackend backend = Spark::RHI::RHIBridge::GetRecommendedBackend();

    bool ok = rhi.bridge.Initialize(static_cast<void*>(hWnd), m_width, m_height, backend,
#ifndef NDEBUG
                                    true
#else
                                    false
#endif
    );

    if (!ok)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "RHI bridge initialization failed");
        return E_FAIL;
    }

    rhi.initialized = true;
    rhi.width = m_width;
    rhi.height = m_height;

    // Create subsystems
    m_textureSystem = std::make_unique<TextureSystem>();
    m_materialSystem = std::make_unique<MaterialSystem>();
    m_lightingSystem = std::make_unique<LightingSystem>();
    m_assetPipeline = std::make_unique<AssetPipeline>();
    m_lightManager = std::make_unique<LightManager>();
    m_renderPipeline = std::make_unique<Spark::Graphics::RenderPipeline>();
    m_renderPipeline->SetGraphicsEngine(this);
    m_postProcessing = std::make_unique<PostProcessingPipeline>();

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Initialized on Linux via RHI (%s)",
                   rhi.bridge.GetBackendName().c_str());

    return S_OK;
}

// ============================================================================
// Shutdown
// ============================================================================

void GraphicsEngine::Shutdown()
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Graphics.RHI", 0.0);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GraphicsEngine::Shutdown (RHI path) — beginning teardown");
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    m_textureSystem.reset();
    m_materialSystem.reset();
    m_lightingSystem.reset();
    m_postProcessing.reset();
    m_assetPipeline.reset();
    m_physicsSystem = nullptr;
    m_lightManager.reset();
    m_postProcessing.reset();
    m_temporalEffects.reset();
    m_shader.reset();

    rhi.bridge.Shutdown();
    rhi.initialized = false;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shutdown complete");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Graphics.RHI", 0.0);
}

// ============================================================================
// Resize
// ============================================================================

HRESULT GraphicsEngine::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return E_INVALIDARG;

    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    if (!rhi.bridge.Resize(width, height))
        return E_FAIL;

    m_width = width;
    m_height = height;
    m_windowWidth = width;
    m_windowHeight = height;
    rhi.width = width;
    rhi.height = height;

    return S_OK;
}

// ============================================================================
// Frame Management
// ============================================================================

void GraphicsEngine::BeginFrame()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    bool expected = false;
    if (!m_frameInProgress.compare_exchange_strong(expected, true))
        return;

    rhi.frameStart = std::chrono::high_resolution_clock::now();

    rhi.bridge.BeginFrame();

    // Clear the back buffer
    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (cmd)
    {
        Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
        Spark::RHI::IRHITexture* depthBuffer = rhi.bridge.GetDepthBuffer();

        if (backBuffer)
        {
            cmd->SetRenderTargets(&backBuffer, 1, depthBuffer);
            cmd->ClearRenderTarget(backBuffer, m_settings.clearColor);
        }
        if (depthBuffer)
        {
            cmd->ClearDepthStencil(depthBuffer, 1.0f, 0);
        }

        // Set viewport
        Spark::RHI::RHIViewport vp;
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(m_width);
        vp.height = static_cast<float>(m_height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        cmd->SetViewport(vp);

        Spark::RHI::RHIScissorRect sr;
        sr.left = 0;
        sr.top = 0;
        sr.right = static_cast<int32_t>(m_width);
        sr.bottom = static_cast<int32_t>(m_height);
        cmd->SetScissorRect(sr);
    }

    m_statistics.drawCalls = 0;
    m_statistics.triangles = 0;
    m_statistics.vertices = 0;
}

void GraphicsEngine::EndFrame()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;
    if (!m_frameInProgress.load())
        return;

    rhi.bridge.EndFrame();
    rhi.bridge.Present(m_settings.vsync);

    // Timing
    auto now = std::chrono::high_resolution_clock::now();
    float frameDelta = std::chrono::duration<float, std::milli>(now - rhi.frameStart).count();
    m_statistics.frameTime = frameDelta;
    m_statistics.cpuTime = frameDelta;

    // FPS calculation (rolling window)
    rhi.accumulatedTime += frameDelta;
    rhi.frameCount++;
    if (rhi.accumulatedTime >= 1000.0f)
    {
        rhi.measuredFps = rhi.frameCount;
        m_statistics.fps = rhi.measuredFps;
        rhi.frameCount = 0;
        rhi.accumulatedTime = 0.0f;
    }

    // Pull RHI statistics
    const auto& rhiStats = rhi.bridge.GetFrameStatistics();
    m_statistics.drawCalls += rhiStats.drawCalls;
    m_statistics.triangles += rhiStats.trianglesRendered;
    m_statistics.vertices += rhiStats.verticesProcessed;
    m_statistics.textureBinds = rhiStats.textureBinds;
    m_statistics.gpuTime = rhiStats.gpuFrameTime;
    m_statistics.totalGPUMemory = rhiStats.gpuMemoryUsed;

    m_frameInProgress.store(false);
}

// ============================================================================
// RenderScene
// ============================================================================

void GraphicsEngine::RenderScene(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                 const std::vector<GameObject*>& objects)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("RenderScene");

    m_statistics.totalObjects = static_cast<uint32_t>(objects.size());
    uint32_t visibleCount = 0;

    for (auto* obj : objects)
    {
        if (!obj)
            continue;
        if (!obj->IsActive() || !obj->IsVisible())
            continue;

        visibleCount++;
        obj->Render(viewMatrix, projMatrix);
        m_statistics.drawCalls++;
    }

    m_statistics.visibleObjects = visibleCount;
    m_statistics.culledObjects = m_statistics.totalObjects - visibleCount;

    cmd->EndEvent();
}

// NOTE: SubmitMeshForRendering and ProcessDrawList are defined above in the
// "ECS MESH DRAW SUBMISSION" section (single definition, spinlock-based).

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

#endif // SPARK_PLATFORM_WINDOWS
