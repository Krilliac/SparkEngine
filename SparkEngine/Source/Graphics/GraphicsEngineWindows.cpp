/**
 * @file GraphicsEngineWindows.cpp
 * @brief Windows/D3D11 central rendering orchestrator for SparkEngine
 *
 * Core lifecycle: construction, initialization, shutdown, frame management,
 * scene rendering dispatch, ECS draw submission, and system accessors.
 * Linux counterpart lives in GraphicsEngineLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
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
#include "UpscalingSystem.h"
#include "VRAMBudgetMonitor.h"
#include "FoliageRenderer.h"

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
#include "ShaderHotReload.h"
#include "RenderTarget.h"
#include "GPUDrivenRenderer.h"
#include "../Physics/PhysicsSystem.h"
#include "../Game/GameObject.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
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
// INITIALIZATION
// ============================================================================

HRESULT GraphicsEngine::Initialize(Spark::NativeWindowHandle hWnd)
// NOTE: Intentionally exceeds 50-line guideline — linear initialization sequence
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

    // Rasterizer (solid/wireframe), depth-stencil, and blend states are now
    // created together in CreateRenderStates() (called below) so the same
    // shared function also equips the device-attach InitializeFromDevice()
    // path with real state objects.

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

    if (m_upscalingSystem)
    {
        if (m_upscalingSystem->Initialize(m_device.Get(), m_context.Get(), m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"UpscalingSystem initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize UpscalingSystem", L"ERROR");
        }
    }

    if (m_vramBudgetMonitor)
    {
        hr = m_vramBudgetMonitor->Initialize(m_device.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize VRAMBudgetMonitor", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"VRAMBudgetMonitor initialized successfully", L"SUCCESS");

            // Connect to TextureSystem for pressure-driven eviction
            if (m_textureSystem)
            {
                m_textureSystem->SetVRAMBudgetMonitor(m_vramBudgetMonitor.get());
            }
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

    // Initialize GPU-driven renderer (Nanite-like culling pipeline)
    if (m_settings.gpuDrivenRendering)
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.Initialize(m_device.Get(), m_context.Get()))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPUDrivenRenderer initialized", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPUDrivenRenderer init failed — falling back to CPU draw", L"WARNING");
        }
    }

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

    // Phase Q: activate the image denoiser. The default backend is
    // SoftwareDenoiser (joint-bilateral filter), which works on every
    // platform with no external SDK. A future pipeline can replace
    // m_denoiser with an OIDN / OptiX instance before the first
    // Execute() call. The initial settings have `enabled = false` so
    // no CPU work happens until a real RT pass toggles it on.
    m_denoiser = std::make_unique<Spark::Graphics::SoftwareDenoiser>();
    Spark::Graphics::DenoiserSettings denoiserSettings;
    denoiserSettings.backend = Spark::Graphics::DenoiserBackend::Software;
    denoiserSettings.quality = Spark::Graphics::DenoiserQuality::Balanced;
    m_denoiser->Initialize(denoiserSettings);

    // Phase S: activate the procedural noise graph. Default output
    // is a SimplexNode so the accessor is useful immediately; terrain
    // / foliage / decoration systems can add more nodes via
    // `GetProceduralNoise()->AddNode(...)` without rebuilding the
    // engine.
    m_proceduralNoise = std::make_unique<Spark::Graphics::NoiseGraph>();
    {
        auto defaultNode = std::make_unique<Spark::Graphics::SimplexNode>();
        auto* nodePtr = m_proceduralNoise->AddNode(std::move(defaultNode));
        m_proceduralNoise->SetOutputNode(nodePtr);
    }

    // Phase T: activate the voxel cone traced GI system with a
    // small 32³ default grid (~130 KB) and `enabled = false`. A
    // future GI render pass that wants full resolution can
    // re-initialise via `GetVCTSystem()->Initialize({...})` with a
    // 128³ grid. Keeping the default small avoids wasting ~9 MB
    // per GraphicsEngine instance for a feature that is opt-in.
    m_vctSystem = std::make_unique<Spark::Graphics::VCTSystem>();
    {
        Spark::Graphics::VCTSettings vctSettings;
        vctSettings.enabled = false;
        vctSettings.voxelResolution = 32;
        vctSettings.worldExtent = 50.0f;
        m_vctSystem->Initialize(vctSettings);
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Graphics", 0.0);
    return S_OK;
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
    m_depthStencilSRV.Reset();
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
// ECS MESH DRAW SUBMISSION
// ============================================================================
// NOTE: SubmitMeshForRendering lives in the shared GraphicsEngineSubmit.cpp
// so Linux/macOS builds can drive the draw list too. ProcessDrawList below
// stays Windows-only — it touches D3D11 constant buffers, the GPU-driven
// renderer, and the asset pipeline's D3D11 loaders.

void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix)
{
    // Swap the draw list with a persistent processing buffer under the spinlock —
    // minimal hold time. std::swap preserves the allocated capacity on both
    // vectors, so after the first frame's capacity is established neither side
    // allocates or frees heap memory on the steady-state submission path.
    {
        SpinlockGuard guard(m_drawListSpinlock);
        std::swap(m_drawList, m_processingDrawList);
    }

    // Alias for readability; m_processingDrawList now owns this frame's commands.
    std::vector<MeshDrawCommand>& localDrawList = m_processingDrawList;

    if (localDrawList.empty())
        return;

    // GPU-driven culling: group draws by mesh and use indirect draw per batch.
    // When enabled, instances sharing the same mesh are culled on the GPU via
    // frustum + HiZ compute shaders, then drawn with DrawIndexedInstancedIndirect.
    // Unique-mesh draws that can't batch fall through to the CPU path below.
    if (m_settings.gpuDrivenRendering && m_assetPipeline)
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.IsInitialized())
        {
            // Sort by mesh path to batch instances of the same mesh
            std::sort(localDrawList.begin(), localDrawList.end(),
                      [](const MeshDrawCommand& a, const MeshDrawCommand& b) { return a.meshPath < b.meshPath; });

            SetBasicShaders();
            size_t i = 0;
            while (i < localDrawList.size())
            {
                // Find the range of commands sharing the same mesh
                size_t batchStart = i;
                std::string_view batchMesh = localDrawList[i].meshPath;
                while (i < localDrawList.size() && localDrawList[i].meshPath == batchMesh)
                    ++i;
                uint32_t batchCount = static_cast<uint32_t>(i - batchStart);

                // Look up the mesh asset for vertex/index buffers and AABB.
                // LoadMesh is cached — the std::string argument is required by the
                // current loader signature but the hot lookup path inside is
                // transparent-hash and allocation-free.
                auto meshAsset = m_assetPipeline->LoadMesh(std::string(batchMesh));
                if (!meshAsset || !meshAsset->GetVertexBuffer() || !meshAsset->GetIndexBuffer())
                    continue;

                const auto& meshData = meshAsset->GetMeshData();
                XMFLOAT3 bbMin = meshData.boundingBoxMin;
                XMFLOAT3 bbMax = meshData.boundingBoxMax;

                // Build per-instance AABBs by transforming the mesh AABB
                std::vector<Spark::Graphics::GPUInstanceAABB> aabbs;
                aabbs.reserve(batchCount);
                for (size_t j = batchStart; j < batchStart + batchCount; ++j)
                {
                    XMMATRIX world = XMLoadFloat4x4(&localDrawList[j].worldMatrix);

                    // Conservative AABB transform: project all 8 corners
                    XMFLOAT3 corners[8] = {
                        {bbMin.x, bbMin.y, bbMin.z}, {bbMax.x, bbMin.y, bbMin.z}, {bbMin.x, bbMax.y, bbMin.z},
                        {bbMax.x, bbMax.y, bbMin.z}, {bbMin.x, bbMin.y, bbMax.z}, {bbMax.x, bbMin.y, bbMax.z},
                        {bbMin.x, bbMax.y, bbMax.z}, {bbMax.x, bbMax.y, bbMax.z},
                    };

                    Spark::Graphics::GPUInstanceAABB aabb;
                    aabb.minX = aabb.minY = aabb.minZ = FLT_MAX;
                    aabb.maxX = aabb.maxY = aabb.maxZ = -FLT_MAX;
                    for (const auto& c : corners)
                    {
                        XMVECTOR pt = XMVector3Transform(XMLoadFloat3(&c), world);
                        XMFLOAT3 tp;
                        XMStoreFloat3(&tp, pt);
                        aabb.minX = std::min(aabb.minX, tp.x);
                        aabb.minY = std::min(aabb.minY, tp.y);
                        aabb.minZ = std::min(aabb.minZ, tp.z);
                        aabb.maxX = std::max(aabb.maxX, tp.x);
                        aabb.maxY = std::max(aabb.maxY, tp.y);
                        aabb.maxZ = std::max(aabb.maxZ, tp.z);
                    }
                    aabbs.push_back(aabb);
                }

                // Bind material from first instance (batch shares mesh, material may vary).
                // string_view overload avoids per-draw-call std::string allocation.
                m_assetPipeline->BindMaterial(localDrawList[batchStart].materialPath);

                // GPU cull + indirect draw
                ID3D11Buffer* vb = meshAsset->GetVertexBuffer();
                ID3D11Buffer* ib = meshAsset->GetIndexBuffer();
                uint32_t vertexStride = static_cast<uint32_t>(sizeof(MeshAssetData::Vertex));
                gpuRenderer.CullAndDraw(aabbs.data(), batchCount, viewMatrix, projMatrix, ib, vb, vertexStride,
                                        meshAsset->GetIndexCount());

                m_statistics.drawCalls += gpuRenderer.GetVisibleCount();
            }
            localDrawList.clear();
            return;
        }
    }

    // CPU draw path: sort by material to minimize state changes
    std::sort(localDrawList.begin(), localDrawList.end(),
              [](const MeshDrawCommand& a, const MeshDrawCommand& b) { return a.materialPath < b.materialPath; });

    // Set up shaders for ECS mesh rendering
    SetBasicShaders();

    std::string_view lastMaterial;
    for (const auto& cmd : localDrawList)
    {
        XMMATRIX world = XMLoadFloat4x4(&cmd.worldMatrix);

        // Update per-object constant buffer with world/view/proj matrices
        UpdateBasicConstants(world, viewMatrix, projMatrix);

        // Bind mesh and material through the asset pipeline, then draw.
        // string_view overloads + transparent-hash lookup keep this allocation-free
        // on the per-draw-call inner loop.
        if (m_assetPipeline)
        {
            m_assetPipeline->BindMesh(cmd.meshPath);
            // Only rebind material when it changes (sorted order)
            if (cmd.materialPath != lastMaterial)
            {
                m_assetPipeline->BindMaterial(cmd.materialPath);
                lastMaterial = cmd.materialPath;
            }
            m_assetPipeline->DrawBoundMesh();
        }

        m_statistics.drawCalls++;
    }

    // Clear the processing buffer once drained so the next frame's swap
    // delivers an empty (but capacity-preserving) buffer back to m_drawList.
    localDrawList.clear();
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


#endif // SPARK_PLATFORM_WINDOWS
