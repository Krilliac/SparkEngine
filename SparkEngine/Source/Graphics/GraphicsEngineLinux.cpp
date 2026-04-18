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
#include "TerrainRenderer.h"
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

// Forward declarations for the file-local platform RT helpers — the
// definitions sit below `AcquireHybridRTBindings` so the Initialize /
// Shutdown / Resize call sites need these prototypes first.
namespace
{
    void CreatePlatformRenderTargets(uint32_t width, uint32_t height);
    void ReleasePlatformRenderTargets();
} // namespace

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

    // Headless mode: when no window is provided, force NullRHI instead of
    // picking up an available GPU backend. Vulkan Lavapipe / OpenGL / etc.
    // can initialize successfully but then hang or misbehave without a
    // surface to present to, so headless tests and tools must stay on
    // NullRHI. The backend fallback inside RHIBridge::Initialize still
    // handles the case where the preferred GPU backend fails for a real
    // window, falling through to NullRHI on its own.
    Spark::RHI::GraphicsBackend backend =
        (hWnd == nullptr) ? Spark::RHI::GraphicsBackend::None : Spark::RHI::RHIBridge::GetRecommendedBackend();

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

    // Create GBuffer + HDR + depth targets through the RHI bridge and register
    // them under the shared slot enum so cross-platform code (HybridRT dispatch,
    // golden-image capture, debug viewers) can pull them by name. Matches the
    // Windows GBuffer layout one-for-one; on NullRHI the device returns valid
    // stub textures and registration is harmless. Failures are tolerated — any
    // slot that doesn't resolve stays nullptr and downstream `IsReady()`
    // guards skip the dispatch cleanly (see HybridRTBindings in
    // GraphicsEngine.h).
    CreatePlatformRenderTargets(m_width, m_height);

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

    // TemporalEffects tracks CPU-side jitter, history, and motion vectors
    // even without a D3D11 device. Windows only calls SetDevice() to opt in
    // to the GPU path. On Linux we init the CPU state only.
    m_temporalEffects = std::make_unique<TemporalEffects>();
    if (!m_temporalEffects->Initialize(m_width, m_height))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "GraphicsEngine (Linux): TemporalEffects::Initialize returned false");
    }

    // ShadowAtlas is pure CPU bookkeeping — allocation tracker + tile LRU.
    m_shadowAtlas = std::make_unique<Spark::Graphics::ShadowAtlas>();
    if (!m_shadowAtlas->Initialize(m_settings.shadowMapSize * 2))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GraphicsEngine (Linux): ShadowAtlas::Initialize returned false");
    }

    // ScreenSpaceEffects generates CPU-side SSAO kernel and noise texture
    // data; GPU resource creation is a stub until SetDevice is called.
    m_screenSpaceEffects = std::make_unique<Spark::Graphics::ScreenSpaceEffects>();
    if (!m_screenSpaceEffects->Initialize(m_width, m_height))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "GraphicsEngine (Linux): ScreenSpaceEffects::Initialize returned false");
    }

    // TerrainRenderer has a Linux-specific no-device Initialize. The CPU
    // tile LRU + heightfield sampling state runs without a GPU.
    m_terrainRenderer = std::make_unique<Spark::Graphics::TerrainRenderer>();
    if (!m_terrainRenderer->Initialize())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "GraphicsEngine (Linux): TerrainRenderer::Initialize returned false");
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

    // Explicit Shutdown() on all subsystems that were initialized in
    // Initialize(), in reverse order. Mirrors the Windows path — destructors
    // alone are insufficient because some subsystems hold references or
    // emit diagnostic logs only from Shutdown.
    if (m_terrainRenderer)
        m_terrainRenderer->Shutdown();
    if (m_screenSpaceEffects)
        m_screenSpaceEffects->Shutdown();
    if (m_shadowAtlas)
        m_shadowAtlas->Shutdown();
    if (m_temporalEffects)
        m_temporalEffects->Shutdown();
    if (m_upscalingSystem)
        m_upscalingSystem->Shutdown();
    if (m_lightManager)
        m_lightManager->Shutdown();
    if (m_postProcessing)
        m_postProcessing->Shutdown();
    if (m_assetPipeline)
        m_assetPipeline->Shutdown();
    if (m_lightingSystem)
        m_lightingSystem->Shutdown();
    if (m_materialSystem)
        m_materialSystem->Shutdown();
    if (m_textureSystem)
        m_textureSystem->Shutdown();

    m_textureSystem.reset();
    m_materialSystem.reset();
    m_lightingSystem.reset();
    m_assetPipeline.reset();
    m_upscalingSystem.reset();
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

    // Deregister + release GBuffer/HDR/depth textures before bridge shutdown —
    // the bridge's registry stores non-owning pointers and must not be left
    // dangling. Calling RegisterRenderTarget(slot, nullptr) clears the slot.
    ReleasePlatformRenderTargets();

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

    // GBuffer/HDR/depth textures are tied to the swapchain dimensions — drop
    // the old ones and create fresh at the new size. Registry pointers are
    // non-owning, so the release helper clears the slots before the
    // underlying textures are freed (contract: see RHIBridge.h comments).
    ReleasePlatformRenderTargets();
    CreatePlatformRenderTargets(m_width, m_height);

    // Propagate the new viewport to every subsystem that tracks resolution.
    // Without this, the subsystems keep their initial m_width/m_height and
    // any subsequent render would use stale data.
    if (m_postProcessing)
        m_postProcessing->Resize(width, height);
    if (m_temporalEffects)
        m_temporalEffects->Resize(width, height);
    if (m_screenSpaceEffects)
        m_screenSpaceEffects->Resize(width, height);
    if (m_lightManager)
        m_lightManager->Resize(width, height);

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
    // 0.5 s) so a fixed nominal delta is both safe and cheap. Previously
    // guarded by `if (m_shader)`, but m_shader is never instantiated on
    // either platform — calling the singleton directly bypasses the
    // dead member and actually runs the file watcher.
    Spark::Graphics::ShaderHotReload::GetInstance().Update(1.0f / 60.0f);

    // Per-frame subsystem updates. These advance async load queues,
    // tile-binning counters, shadow cache frame state, and temporal
    // effect history. Without them, the subsystems silently stall.
    const float kNominalDeltaTime = 1.0f / 60.0f;

    if (m_assetPipeline)
        m_assetPipeline->Update(kNominalDeltaTime);

    if (m_lightingSystem)
    {
        // Identity view/proj in headless mode — the lighting system reads
        // the view matrix only to extract camera position from its inverse.
        DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
        m_lightingSystem->Update(kNominalDeltaTime, identity, identity);
    }

    if (m_temporalEffects)
        m_temporalEffects->Update(kNominalDeltaTime);

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

    // Post-processing runs after scene rendering and before Present.
    // Without this call, all 16 effect passes (Bloom, DoF, Tonemapping,
    // ColorGrading, etc.) were silently skipped on Linux.
    if (m_postProcessing && m_postProcessing->IsInitialized())
    {
        m_postProcessing->Process(1.0f / 60.0f);
    }

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

// SubmitMeshForRendering lives in the shared GraphicsEngineSubmit.cpp so
// Linux/macOS builds can drive the draw list too. ProcessDrawList (D3D11-
// specific) stays Windows-only; on Linux/macOS the draw list accumulates
// but is consumed by the RHI bridge path or the Metal RT scene feeder
// (`Graphics/HybridRT/RTSceneFeeder.h`).

// ============================================================================
// HybridRT GBuffer binding — Linux/macOS
// ============================================================================
// Reads the GBuffer/HDR textures from the RHI bridge's render-target
// registry. The rendering layer is expected to have registered them
// after creation (matched slot numbers in RenderTargetSlot). Anything
// still unregistered stays nullptr; DispatchHybridRTPass skips when
// IsReady() returns false, so partial registration degrades cleanly.

Spark::Graphics::HybridRTBindings GraphicsEngine::AcquireHybridRTBindings()
{
    Spark::Graphics::HybridRTBindings bindings;

    // The Linux/macOS RHI bridge lives in the LinuxRHIState singleton — the
    // GraphicsEngine::m_rhiBridge member is never populated on this branch
    // (it's a Windows-only alias). Source the bridge directly from GetRHI()
    // so unregistered slots degrade to nullptr and DispatchHybridRTPass's
    // `IsReady()` guard skips the pass cleanly.
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return bindings;

    using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
    bindings.normals = rhi.bridge.GetRenderTarget(Slot::GBufferNormals);
    bindings.depth = rhi.bridge.GetRenderTarget(Slot::DepthStencil);
    bindings.albedo = rhi.bridge.GetRenderTarget(Slot::GBufferAlbedo);
    bindings.lighting = rhi.bridge.GetRenderTarget(Slot::HDRLighting);
    return bindings;
}

// ============================================================================
// Platform render target helpers (Linux/macOS) — file-local
// ============================================================================
// Create/destroy the GBuffer + HDR + Depth textures and keep the RHI bridge's
// render-target registry in sync. On Windows this is the D3D11 ComPtr path in
// `CreateAdvancedRenderTargets`; here it uses `RHIBridge::CreateTexture2D` /
// `CreateDepthBuffer` so every backend (Vulkan, OpenGL, Metal, NullRHI) gets
// the same slot layout.
//
// Static free functions (not `GraphicsEngine` members) so the header doesn't
// need a new declaration — the Linux state they touch lives in `LinuxRHIState`
// and they're only called from this TU.

namespace
{
    void CreatePlatformRenderTargets(uint32_t width, uint32_t height)
    {
        auto& rhi = GetRHI();
        if (!rhi.initialized)
            return;

        auto* device = rhi.bridge.GetDevice();
        if (!device)
            return;

        using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
        using Spark::RHI::PixelFormat;
        using Spark::RHI::RHITextureUsage;

        // GBuffer[0]: Albedo — RGBA8 color write + SRV read.
        rhi.gBufferAlbedo = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R8G8B8A8_UNORM);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferAlbedo, rhi.gBufferAlbedo.get());

        // GBuffer[1]: Normals — R16G16B16A16F matches the Windows
        // `WrapNativeTexture` format in GraphicsEngineWindows::AcquireHybridRTBindings
        // so downstream shaders don't need a platform-specific variant.
        rhi.gBufferNormals = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R16G16B16A16_FLOAT);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferNormals, rhi.gBufferNormals.get());

        // GBuffer[2]: Material (roughness / metallic / AO / reserved in RGBA8).
        rhi.gBufferMaterial = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R8G8B8A8_UNORM);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMaterial, rhi.gBufferMaterial.get());

        // GBuffer[3]: Motion vectors in RG16F. Unused by default on NullRHI
        // but keeping the slot populated means TAA / temporal passes can look
        // it up without extra null checks.
        rhi.gBufferMotion = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R16G16_FLOAT);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMotion, rhi.gBufferMotion.get());

        // Depth stencil — RHI-side convenience helper sets the DepthStencil
        // usage flag for us. Must match the swapchain's depth format on real
        // backends; NullRHI accepts anything.
        rhi.depthStencil = rhi.bridge.CreateDepthBuffer(width, height, PixelFormat::D24_UNORM_S8_UINT);
        rhi.bridge.RegisterRenderTarget(Slot::DepthStencil, rhi.depthStencil.get());

        // HDR lighting — R16G16B16A16F with RenderTarget|ShaderResource|UnorderedAccess.
        // The UAV flag is what lets the HybridRT compute pass write into it;
        // the default `CreateRenderTarget` helper only asks for RT|SRV, so we
        // go through `CreateTexture2D` with explicit usage flags.
        rhi.hdrLighting = rhi.bridge.CreateTexture2D(width, height, PixelFormat::R16G16B16A16_FLOAT,
                                                     RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource |
                                                         RHITextureUsage::UnorderedAccess,
                                                     nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::HDRLighting, rhi.hdrLighting.get());
    }

    void ReleasePlatformRenderTargets()
    {
        auto& rhi = GetRHI();
        if (!rhi.initialized)
            return;

        using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
        rhi.bridge.RegisterRenderTarget(Slot::GBufferAlbedo, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferNormals, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMaterial, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMotion, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::DepthStencil, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::HDRLighting, nullptr);

        rhi.gBufferAlbedo.reset();
        rhi.gBufferNormals.reset();
        rhi.gBufferMaterial.reset();
        rhi.gBufferMotion.reset();
        rhi.depthStencil.reset();
        rhi.hdrLighting.reset();
    }
} // namespace

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
