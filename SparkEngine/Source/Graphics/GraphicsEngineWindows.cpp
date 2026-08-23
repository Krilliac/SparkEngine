/**
 * @file GraphicsEngineWindows.cpp
 * @brief Windows/D3D11 central rendering orchestrator for SparkEngine
 *
 * Core lifecycle: construction, device-attach initialization, shutdown, and
 * resize. Windowed initialization lives in GraphicsEngineWindowsInit.cpp,
 * device-lost recovery in GraphicsEngineWindowsDeviceLost.cpp, the per-frame
 * rendering path in GraphicsEngineWindowsFrame.cpp, draw-list processing in
 * GraphicsEngineWindowsDrawList.cpp, and system accessors in
 * GraphicsEngineWindowsAccessors.cpp. Linux counterpart lives in
 * GraphicsEngineLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "UpscalingSystem.h"
#include "VRAMBudgetMonitor.h"

#include "TemporalEffects.h"
#include "LightManager.h"
#include "PostProcessingPipeline.h"
#include "ShadowAtlas.h"
#include "ScreenSpaceEffects.h"
#include "TerrainRenderer.h"
using Spark::Graphics::PostProcessingPipeline;
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#endif
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
#include <memory>
#include <string>

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
        m_upscalingSystem = std::make_unique<UpscalingSystem>();
        m_vramBudgetMonitor = std::make_unique<VRAMBudgetMonitor>();

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
// DEVICE-ATTACH INITIALIZATION (no window / no swapchain)
// ============================================================================

HRESULT GraphicsEngine::InitializeFromDevice(ID3D11Device* device, ID3D11DeviceContext* context)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::InitializeFromDevice (attach mode) started.", L"INFO");

    if (!device || !context)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: null device/context in GraphicsEngine::InitializeFromDevice", L"ERROR");
        return E_INVALIDARG;
    }

    // Mark attach mode up front so any swapchain-dependent entry point
    // (BeginFrame/EndFrame/Resize) that might run concurrently or be called
    // by mistake sees the flag and safely no-ops instead of touching a
    // nonexistent swapchain/backbuffer.
    m_attachedMode = true;

    // Adopt the caller-owned device/context. ComPtr's raw-pointer
    // constructor AddRefs, mirroring the ownership CreateDeviceAndSwapChain()
    // establishes for the windowed path (this GraphicsEngine does NOT take
    // exclusive ownership of a device it didn't create, but it does keep a
    // live reference for the duration it's in use).
    ComPtr<ID3D11Device> baseDevice(device);
    ComPtr<ID3D11DeviceContext> baseContext(context);

    HRESULT hr = baseDevice.As(&m_device);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"InitializeFromDevice: failed to query ID3D11Device1", L"ERROR");
        return hr;
    }

    hr = baseContext.As(&m_context);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"InitializeFromDevice: failed to query ID3D11DeviceContext1", L"ERROR");
        return hr;
    }

    // No swapchain, backbuffer RTV, or depth buffer are created here — the
    // caller owns and binds its own render target(s). The basic-shader draw
    // path (SetBasicShaders/UpdateBasicConstants/SetBasicTexture/
    // GetOrLoadTextureSRV/GetOrLoadBasicMaterial) only depends on m_device/
    // m_context, so InitializeBasicShaders() alone is sufficient here — it's
    // the exact same call the windowed Initialize(hWnd) path makes after
    // device/swapchain/render-target setup.
    hr = InitializeBasicShaders();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"InitializeFromDevice: InitializeBasicShaders failed", L"ERROR");
        return hr;
    }

    // Create the same rasterizer/depth-stencil/blend state objects the
    // windowed path creates, so ApplyBasicRenderStates() (called at the top
    // of Spark::RenderWorldBasic()) has real states to bind in attach mode
    // too — not just when a swapchain exists. Non-fatal on failure: the
    // caller's own device-default state still lets basic-shader draws work,
    // just without an explicit rasterizer/depth/blend guarantee.
    hr = CreateRenderStates();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"InitializeFromDevice: CreateRenderStates failed (non-fatal)", L"WARNING");
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::InitializeFromDevice complete - attach-mode rendering ready.",
                             L"SUCCESS");
    return S_OK;
}

// ============================================================================
// SHUTDOWN
// ============================================================================

void GraphicsEngine::Shutdown()
// NOTE: Intentionally exceeds 50-line guideline — linear initialization sequence
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

    if (m_vramBudgetMonitor)
    {
        m_vramBudgetMonitor->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"VRAMBudgetMonitor shutdown complete", L"INFO");
    }

    // Shutdown GPU-driven renderer
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.IsInitialized())
        {
            gpuRenderer.Shutdown();
            LOG_TO_CONSOLE_IMMEDIATE(L"GPUDrivenRenderer shutdown complete", L"INFO");
        }
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

    // Phase Q: tear down the image denoiser. Shutdown() drops the
    // internal output buffer; resetting the unique_ptr releases the
    // polymorphic backend so a future swap to OIDN / OptiX can
    // construct a fresh instance on the next Initialize.
    if (m_denoiser)
    {
        m_denoiser->Shutdown();
        m_denoiser.reset();
    }

    // Phase S: tear down the procedural noise graph. The unique_ptr
    // releases all owned nodes via the graph's vector destructor.
    m_proceduralNoise.reset();

    // Phase T: tear down the VCT system. Shutdown() clears the
    // voxel grid (destroys the mip chain + base data); resetting
    // the unique_ptr releases the wrapper itself. Calling Shutdown
    // on an uninitialised system is safe.
    if (m_vctSystem)
    {
        m_vctSystem->Shutdown();
        m_vctSystem.reset();
    }

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
    m_depthStencilSRV.Reset();
    m_depthStencilView.Reset();
    m_backBufferSRV.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine shutdown complete.", L"INFO");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Graphics", 0.0);
}

// ============================================================================
// RESIZE
// ============================================================================

HRESULT GraphicsEngine::Resize(uint32_t width, uint32_t height)
{
    if (m_attachedMode)
    {
        // No swapchain/backbuffer to resize in attach mode — the caller
        // owns and manages its own render target sizing.
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Resize() called on an attach-mode GraphicsEngine — skipping");
        return S_OK;
    }

    if (width == 0 || height == 0)
        return E_INVALIDARG;

    OnResize(width, height);
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS
