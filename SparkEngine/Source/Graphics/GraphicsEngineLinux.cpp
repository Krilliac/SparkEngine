/**
 * @file GraphicsEngineLinux.cpp
 * @brief Linux RHI-bridge rendering orchestrator for SparkEngine
 *
 * Routes rendering through the RHI bridge (GraphicsEngineRHI.h).
 * Windows counterpart lives in GraphicsEngineWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux implementation — routes rendering through the RHI bridge
// ============================================================================
#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "UpscalingSystem.h"
#include "VRAMBudgetMonitor.h"
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
// Phase U: activated Tier 2 graphics orphan — process-wide shader file
// watcher. Pumped from the Linux BeginFrame so headless / RHI builds
// share the same per-frame hot-reload poll that the Windows branch gets
// via Shader::HotReloadShaders.
#include "ShaderHotReload.h"
#include "Shader.h"
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
    m_upscalingSystem = std::make_unique<UpscalingSystem>();
    m_vramBudgetMonitor = std::make_unique<VRAMBudgetMonitor>();
    m_lightManager = std::make_unique<LightManager>();
    m_renderPipeline = std::make_unique<Spark::Graphics::RenderPipeline>();
    m_renderPipeline->SetGraphicsEngine(this);
    m_postProcessing = std::make_unique<PostProcessingPipeline>();

    // Initialize subsystems in headless mode. All four accept null
    // device/context on Linux; the Linux-specific implementations operate on
    // CPU-side state only and don't touch the D3D11 stubs.
    if (m_textureSystem)
    {
        HRESULT hr = m_textureSystem->Initialize(nullptr, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): TextureSystem::Initialize failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    if (m_materialSystem)
    {
        HRESULT hr = m_materialSystem->Initialize(nullptr, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): MaterialSystem::Initialize failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    if (m_lightingSystem)
    {
        HRESULT hr = m_lightingSystem->Initialize(nullptr, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): LightingSystem::Initialize failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }
    if (m_assetPipeline)
    {
        HRESULT hr = m_assetPipeline->Initialize(nullptr, nullptr);
        if (FAILED(hr))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): AssetPipeline::Initialize failed (hr=0x%08X)",
                           static_cast<unsigned>(hr));
        }
    }

    // PostProcessingPipeline has no device requirement for its CPU-side
    // state (temporal filter, volume manager, RT handle system). The
    // GPU-backed effects are behind #ifdef SPARK_PLATFORM_WINDOWS guards
    // that check m_device, so a null device is safe in headless mode.
    if (m_postProcessing)
    {
        if (!m_postProcessing->Initialize(m_width, m_height))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): PostProcessingPipeline::Initialize returned false");
        }
    }

    // LightManager has no device dependency — pure CPU tile binning + shadow
    // atlas slot tracking. Safe to initialize in headless mode.
    if (m_lightManager)
    {
        if (!m_lightManager->Initialize(m_width, m_height, 16))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): LightManager::Initialize returned false");
        }
    }

    // UpscalingSystem's Linux CreateGPUResources is a no-op (returns true),
    // so Initialize with null device/context succeeds and the system tracks
    // render/display resolution on the CPU side.
    if (m_upscalingSystem)
    {
        if (!m_upscalingSystem->Initialize(nullptr, nullptr, m_width, m_height))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "GraphicsEngine (Linux): UpscalingSystem::Initialize returned false");
        }
    }

    // Phase Q: mirror the Windows denoiser activation so Linux /
    // headless builds have the same live IDenoiser instance and
    // tests exercising GraphicsEngine directly see consistent state.
    m_denoiser = std::make_unique<Spark::Graphics::SoftwareDenoiser>();
    Spark::Graphics::DenoiserSettings denoiserSettings;
    denoiserSettings.backend = Spark::Graphics::DenoiserBackend::Software;
    denoiserSettings.quality = Spark::Graphics::DenoiserQuality::Balanced;
    if (!m_denoiser->Initialize(denoiserSettings))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Denoiser::Initialize failed — continuing with stub");
    }

    // Phase S: mirror the procedural noise graph activation on
    // Linux / headless so tests exercising GraphicsEngine see the
    // same default output node.
    m_proceduralNoise = std::make_unique<Spark::Graphics::NoiseGraph>();
    {
        auto defaultNode = std::make_unique<Spark::Graphics::SimplexNode>();
        auto* nodePtr = m_proceduralNoise->AddNode(std::move(defaultNode));
        m_proceduralNoise->SetOutputNode(nodePtr);
    }

    // Phase T: mirror the VCT system activation on the Linux
    // path. Same small default grid so headless builds keep a
    // trivial memory footprint.
    m_vctSystem = std::make_unique<Spark::Graphics::VCTSystem>();
    {
        Spark::Graphics::VCTSettings vctSettings;
        vctSettings.enabled = false;
        vctSettings.voxelResolution = 32;
        vctSettings.worldExtent = 50.0f;
        if (!m_vctSystem->Initialize(vctSettings))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "VCTSystem::Initialize failed — continuing without VCT");
        }
    }

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
    m_vramBudgetMonitor.reset();
    m_physicsSystem = nullptr;
    m_lightManager.reset();
    m_postProcessing.reset();
    m_temporalEffects.reset();
    m_shader.reset();

    // Phase Q: tear down the denoiser on the Linux path too.
    if (m_denoiser)
    {
        m_denoiser->Shutdown();
        m_denoiser.reset();
    }

    // Phase S: drop the procedural noise graph (Linux stub path).
    m_proceduralNoise.reset();

    // Phase T: tear down the VCT system (Linux stub path).
    if (m_vctSystem)
    {
        m_vctSystem->Shutdown();
        m_vctSystem.reset();
    }

    rhi.bridge.Shutdown();
    rhi.initialized = false;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shutdown complete");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Graphics.RHI", 0.0);
}

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

    // Update VRAM budget monitor (lightweight query)
    if (m_vramBudgetMonitor)
        m_vramBudgetMonitor->Update();

    // Phase U: pump the Spark::Graphics::ShaderHotReload singleton each
    // frame so runtime shader hot-reload runs on Linux and headless
    // builds. The singleton has its own poll-interval gating (default
    // 0.5 s) so a fixed nominal delta is both safe and cheap.
    if (m_shader)
        m_shader->HotReloadShaders();

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


#endif // !SPARK_PLATFORM_WINDOWS
