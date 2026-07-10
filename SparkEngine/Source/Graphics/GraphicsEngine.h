/**
 * @file GraphicsEngine.h
 * @brief Advanced DirectX 11 graphics engine with AAA features and console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class manages the entire DirectX 11 rendering pipeline with advanced features
 * including PBR materials, deferred rendering, post-processing, temporal effects,
 * texture streaming, asset pipeline, physics integration, and comprehensive 
 * console integration for real-time graphics debugging.
 */

#pragma once

#include "../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include "../Core/framework.h"
#include "GraphicsEngineTypes.h"
#include "Shader.h" // PerObjectConstants and PerFrameConstants struct definitions
#include "DrawSortKey.h"
#include "PipelineStateCache.h"
#include "RenderTargetPool.h"
#include "BVHAccelerator.h"
#include "GPUSceneBuffer.h"
#include "ConstantBufferRing.h"
#include "GPUDebugMarkers.h"
#include "MakeDesc.h"
#include "TerrainRenderer.h"
#include "GPUTimestampQuery.h"
// Phase Q: activated Tier 2 graphics orphan — abstract denoiser
// interface plus SoftwareDenoiser fallback. Pure CPU (joint bilateral
// filter), no external SDK dependency, runs on every platform.
#include "DenoiserInterface.h"
// Phase S: activated Tier 2 graphics orphan — SIMD-accelerated
// procedural noise graph (Simplex/Perlin/Cellular/FBM/DomainWarp/
// Combiner nodes with batch evaluation). Pure CPU, runtime SIMD
// detection, runs on every platform.
#include "FastNoise2SIMD.h"
// Phase T: activated Tier 2 graphics orphan — voxel-cone-traced
// global illumination. Pure CPU VoxelGrid + VCTSystem (no D3D11
// dependency); the mip chain, cone trace, and tangent-frame math
// all run on every platform. Default settings disable the pipeline
// so the grid allocation is tiny (~130 KB at 32³) until a future
// render pass opts in.
#include "VoxelConeTracing.h"
#include "RHI/RHIBridge.h"
#ifdef SPARK_HARDWARE_RT
#include "RHI/DXRSupport.h"
#endif
#include "RenderPipeline.h"
#include <functional>
#include <mutex>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string_view>
#include <atomic> // Thread-safe frame state management

using Microsoft::WRL::ComPtr;

// Forward declarations for engine systems
class RenderTarget;
class MaterialSystem;
class LightManager;
namespace Spark::Graphics
{
    class PostProcessingPipeline;
    class HybridRTManager;
    class ShadowAtlas;
    class ScreenSpaceEffects;
} // namespace Spark::Graphics
#include "TemporalEffects.h" // Full definition needed for TAASettings struct
class TextureSystem;
class LightingSystem;
class AssetPipeline;
class UpscalingSystem;
class VRAMBudgetMonitor;
class PhysicsSystem;
class GameObject;
class Shader;

namespace Spark::RHI
{
    class IRHITexture;
    class IRHICommandList;
} // namespace Spark::RHI

namespace Spark::Graphics
{
    /**
 * @brief GBuffer + HDR render target handles produced by the platform
 *        pipeline and consumed by the shared HybridRT dispatch helper.
 *
 * On Windows the handles come from wrapping `ComPtr<ID3D11Texture2D>`
 * objects via `IRHIDevice::WrapNativeTexture`. On Linux/macOS the RHI
 * bridge is expected to hand out `IRHITexture*` directly; until that
 * retrieval is implemented the bindings are left empty and the HybridRT
 * Execute call is skipped (the pass becomes a no-op instead of crashing).
 *
 * The struct owns the handles — destruction releases the wrappers in
 * LIFO order, matching D3D11 ComPtr teardown semantics.
 */
    struct HybridRTBindings
    {
        // Non-owning pointers consumed by HybridRTManager::Execute.
        // Either come from the RHIBridge render-target registry (Linux/
        // macOS — platform creates RHI textures once, registers them, the
        // bindings just reference) or alias into `owned` below (Windows —
        // wraps D3D11 ComPtr textures per-frame, needs to keep the
        // wrappers alive for the duration of the dispatch).
        Spark::RHI::IRHITexture* normals = nullptr;
        Spark::RHI::IRHITexture* depth = nullptr;
        Spark::RHI::IRHITexture* albedo = nullptr;
        Spark::RHI::IRHITexture* lighting = nullptr;

        /// Keeps wrapped textures alive for the lifetime of this
        /// bindings object. Windows AcquireHybridRTBindings populates
        /// this with the per-frame `WrapNativeTexture` results and
        /// aliases the raw pointers above into these `unique_ptr`s.
        /// Linux/macOS leaves it empty — the bridge registry owns the
        /// textures long-term and the raw pointers reference directly.
        std::vector<std::unique_ptr<Spark::RHI::IRHITexture>> owned;

        /// True if at least the four primary inputs (normals, depth,
        /// albedo, lighting) resolved — otherwise the dispatch is skipped.
        bool IsReady() const { return normals && depth && albedo && lighting; }
    };
} // namespace Spark::Graphics

/**
 * @brief Advanced DirectX 11 graphics engine with AAA features
 * 
 * Comprehensive graphics engine supporting modern rendering techniques,
 * multiple rendering pipelines, advanced post-processing, and extensive
 * console integration for real-time debugging and performance tuning.
 * 
 * Integrates all advanced systems:
 * - TextureSystem for advanced texture management and streaming
 * - MaterialSystem for PBR materials and shader management
 * - LightingSystem for advanced lighting and shadow mapping
 * - PostProcessingPipeline for HDR and visual effects
 * - AssetPipeline for model loading and asset streaming
 * - PhysicsSystem for physics simulation integration
 */
/// @brief RAII guard for std::atomic_flag spinlocks — ensures release even on exception.
class SpinlockGuard
{
  public:
    explicit SpinlockGuard(std::atomic_flag& flag) noexcept : m_flag(flag)
    {
        while (m_flag.test_and_set(std::memory_order_acquire))
        {
        }
    }
    ~SpinlockGuard() noexcept { m_flag.clear(std::memory_order_release); }

    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;

  private:
    std::atomic_flag& m_flag;
};

class GraphicsEngine
{
    // RenderPipeline delegates to private render sub-passes (FillGBuffer,
    // LightingPass, RenderPostProcessing, etc.) when running the render-
    // graph-based pipeline.
    friend class Spark::Graphics::RenderPipeline;

  public:
    /**
     * @brief Default constructor
     */
    GraphicsEngine();

    /**
     * @brief Destructor
     */
    ~GraphicsEngine();

    /**
     * @brief Initialize the graphics engine with advanced features
     * @param hWnd Handle to the window for rendering
     * @return HRESULT indicating success or failure of initialization
     */
    HRESULT Initialize(Spark::NativeWindowHandle hWnd);

    /**
     * @brief Initialize the basic-shader render path (shaders, constant buffers,
     *        sampler, default white texture, texture/material caches) on an
     *        EXISTING device/context. Does NOT create a device or swapchain.
     *
     * Lets a caller that owns its own D3D11 device (e.g. an editor) drive
     * Spark::RenderWorldBasic() through this GraphicsEngine without going
     * through the windowed Initialize(hWnd) path. BeginFrame()/EndFrame()/
     * Present() are not used in this mode — the caller owns and binds its
     * own render target(s) before calling into the basic-shader draw API
     * (SetBasicShaders/UpdateBasicConstants/SetBasicTexture/etc.) directly.
     *
     * @param device  Existing D3D11 device (must support ID3D11Device1).
     * @param context Existing immediate context (must support ID3D11DeviceContext1).
     * @return S_OK on success.
     */
    HRESULT InitializeFromDevice(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Clean up all DirectX resources
     */
    void Shutdown();

    /**
     * @brief Resize the rendering surface
     * @param width New width
     * @param height New height
     */
    HRESULT Resize(uint32_t width, uint32_t height);

    /**
     * @brief Begin a new rendering frame with advanced setup
     */
    void BeginFrame();

    /**
     * @brief Complete the current frame and present to screen
     */
    void EndFrame();

    /**
     * @brief Render the scene with advanced pipeline
     * @param viewMatrix View transformation matrix
     * @param projMatrix Projection transformation matrix
     * @param objects List of objects to render
     */
    void RenderScene(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects);

    /**
     * @brief Handle window resize events with advanced buffer management
     * @param width New window width in pixels
     * @param height New window height in pixels
     */
    void OnResize(unsigned int width, unsigned int height);

    // ========================================================================
    // ECS MESH DRAW SUBMISSION
    // ========================================================================

    /**
     * @brief Data for a single mesh draw command submitted by the ECS RenderSystem.
     *
     * Accumulated each frame via SubmitMeshForRendering() and consumed by
     * ProcessDrawList() during the render pass.
     *
     * Uses string_view references into component storage to avoid per-entity
     * string copies. The views are valid for the duration of the frame since
     * ECS component data is stable within a frame.
     */
    struct MeshDrawCommand
    {
        std::string_view meshPath;
        std::string_view materialPath;
        DirectX::XMFLOAT4X4 worldMatrix;
        bool castShadows = true;
    };

    /**
     * @brief Submit a mesh for deferred rendering during the current frame.
     *
     * Called by RenderSystem::Update() for each visible entity. The command is
     * stored in a per-frame draw list that is consumed by ProcessDrawList().
     * Uses string_view to avoid per-entity string copies — callers must ensure
     * the referenced strings outlive the current frame.
     *
     * @param meshPath      Asset path to the mesh resource (view into component storage).
     * @param materialPath  Asset path to the material resource (view into component storage).
     * @param worldMatrix   World transformation matrix for the mesh instance.
     * @param castShadows   Whether this mesh should be included in the shadow pass.
     */
    void SubmitMeshForRendering(std::string_view meshPath, std::string_view materialPath,
                                const DirectX::XMMATRIX& worldMatrix, bool castShadows);

    /**
     * @brief Process and render all meshes submitted via SubmitMeshForRendering().
     *
     * Iterates the per-frame draw list, binds meshes and materials through the
     * AssetPipeline, updates per-object constants, and issues draw calls. The
     * draw list is cleared after processing.
     *
     * @param viewMatrix  Camera view matrix for the current frame.
     * @param projMatrix  Camera projection matrix for the current frame.
     */
    void ProcessDrawList(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix);

    /**
     * @brief Dispatch the HybridRT post-lighting pass (reflections + shadows + AO + GI).
     *
     * Shared across Windows/Linux/macOS. Computes camera/light uniforms,
     * calls `AcquireHybridRTBindings()` to grab the GBuffer/HDR handles
     * for the current platform, then issues `HybridRTManager::Execute`.
     * Skips cleanly (no dispatch) when `m_hybridRT` is null or the
     * bindings aren't ready yet — lets Linux/macOS fall through to
     * SDFGI / the software path until the RHI bridge exposes GBuffer
     * textures.
     *
     * Intended call site: after the lighting pass, before the
     * transparent forward pass.
     */
    void DispatchHybridRTPass(Spark::RHI::IRHICommandList* cmd, const DirectX::XMMATRIX& viewMatrix,
                              const DirectX::XMMATRIX& projMatrix);

    /**
     * @brief Acquire per-platform GBuffer/HDR texture handles for the
     *        HybridRT pass. Windows wraps D3D11 `ComPtr<ID3D11Texture2D>`
     *        via `IRHIDevice::WrapNativeTexture`; Linux/macOS returns
     *        empty bindings for now (the RHI bridge does not yet expose
     *        GBuffer textures — follow-up).
     */
    Spark::Graphics::HybridRTBindings AcquireHybridRTBindings();

    /**
     * @brief Return the per-frame draw list (read-only, cold path).
     * @return  Copy of the current frame's draw commands.
     */
    std::vector<MeshDrawCommand> GetDrawList() const
    {
        SpinlockGuard guard(m_drawListSpinlock);
        return m_drawList;
    }

    /**
     * @brief Clear all pending draw commands without processing them.
     */
    void ClearDrawList()
    {
        SpinlockGuard guard(m_drawListSpinlock);
        m_drawList.clear();
    }

    // ========================================================================
    // ADVANCED SYSTEM ACCESSORS
    // ========================================================================

    TextureSystem* GetTextureSystem() const;
    MaterialSystem* GetMaterialSystem() const;
    LightingSystem* GetLightingSystem() const;
    Spark::Graphics::PostProcessingPipeline* GetPostProcessingPipeline() const;
    TemporalEffects* GetTemporalEffects() const { return m_temporalEffects.get(); }
    Spark::Graphics::ShadowAtlas* GetShadowAtlas() const { return m_shadowAtlas.get(); }
    Spark::Graphics::ScreenSpaceEffects* GetScreenSpaceEffects() const { return m_screenSpaceEffects.get(); }
    Spark::Graphics::TerrainRenderer* GetTerrainRenderer() const { return m_terrainRenderer.get(); }
    AssetPipeline* GetAssetPipeline() const;
    UpscalingSystem* GetUpscalingSystem() const { return m_upscalingSystem.get(); }
    VRAMBudgetMonitor* GetVRAMBudgetMonitor() const { return m_vramBudgetMonitor.get(); }

    // ========================================================================
    // RHI BRIDGE ACCESS
    // ========================================================================

    /**
     * @brief Get the RHI bridge for backend-agnostic rendering
     *
     * The RHI bridge provides access to the abstract rendering hardware
     * interface, enabling code to issue draw calls through IRHIDevice
     * rather than directly using D3D11 APIs. This is the recommended
     * path for new rendering code.
     *
     * @return Pointer to the RHI bridge, or nullptr if not initialized
     */
    Spark::RHI::RHIBridge* GetRHIBridge() const { return m_rhiBridge.get(); }

    /**
     * @brief Get the active RHI device for resource creation
     *
     * Convenience accessor that returns the underlying IRHIDevice from
     * the RHI bridge. Returns nullptr if the bridge is not initialized.
     */
    Spark::RHI::IRHIDevice* GetRHIDevice() const;

    /**
     * @brief Check if RHI backend is available
     * @return true if the RHI bridge has been initialized
     */
    bool IsRHIAvailable() const { return m_rhiBridge != nullptr; }

    // ========================================================================
    // RENDERER INTEGRATION SYSTEMS
    // ========================================================================

#ifdef SPARK_PLATFORM_WINDOWS
    /** @brief Get the pipeline state cache for hash-based D3D11 state deduplication. */
    Spark::Graphics::D3D11PipelineStateCache* GetPipelineStateCache() { return &m_pipelineStateCache; }
    const Spark::Graphics::D3D11PipelineStateCache* GetPipelineStateCache() const { return &m_pipelineStateCache; }

    /** @brief Get the render target pool for transient RT recycling. */
    Spark::Graphics::RenderTargetPool* GetRenderTargetPool() { return &m_renderTargetPool; }
    const Spark::Graphics::RenderTargetPool* GetRenderTargetPool() const { return &m_renderTargetPool; }

    /** @brief Get the GPU scene buffer for persistent instance data. */
    Spark::Graphics::GPUSceneBuffer* GetGPUSceneBuffer() { return &m_gpuSceneBuffer; }
    const Spark::Graphics::GPUSceneBuffer* GetGPUSceneBuffer() const { return &m_gpuSceneBuffer; }

    /** @brief Get the BVH accelerator for hierarchical frustum culling. */
    Spark::Graphics::BVHAccelerator* GetBVHAccelerator() { return &m_bvhAccelerator; }
    const Spark::Graphics::BVHAccelerator* GetBVHAccelerator() const { return &m_bvhAccelerator; }

    /** @brief Get the constant buffer ring for dynamic CB sub-allocation. */
    Spark::Graphics::ConstantBufferRing* GetConstantBufferRing() { return &m_constantBufferRing; }
    const Spark::Graphics::ConstantBufferRing* GetConstantBufferRing() const { return &m_constantBufferRing; }

    /** @brief Get the GPU debug markers for PIX/RenderDoc annotations. */
    Spark::Graphics::GPUDebugMarkers* GetGPUDebugMarkers() { return &m_gpuDebugMarkers; }
    const Spark::Graphics::GPUDebugMarkers* GetGPUDebugMarkers() const { return &m_gpuDebugMarkers; }

    /** @brief Get the GPU timestamp query system for per-pass timing. */
    Spark::Graphics::GPUTimestampQuery* GetGPUTimestampQuery() { return &m_gpuTimestampQuery; }
    const Spark::Graphics::GPUTimestampQuery* GetGPUTimestampQuery() const { return &m_gpuTimestampQuery; }

    /** @brief Get the depth buffer SRV for HiZ construction and post-process reads. */
    ID3D11ShaderResourceView* GetDepthSRV() const { return m_depthStencilSRV.Get(); }
#endif // SPARK_PLATFORM_WINDOWS

    // ========================================================================
    // Phase Q: DenoiserInterface accessor
    // ========================================================================

    /**
     * @brief Get the engine's image denoiser (Phase Q activation).
     *
     * The `GraphicsEngine` owns one `IDenoiser` instance, default-
     * constructed as a `SoftwareDenoiser` (joint-bilateral fallback
     * that works on every platform with no external SDK). A future
     * ray-traced AO / GI / reflections pass can register its noisy
     * color buffer + optional albedo / normal guides through this
     * accessor, call `Execute()`, and read back the denoised output.
     *
     * The accessor returns a raw pointer (not null after Initialize)
     * so a future swap to OIDN / OptiX is a one-line replacement
     * inside `GraphicsEngine::Initialize`.
     */
    Spark::Graphics::IDenoiser* GetDenoiser() { return m_denoiser.get(); }
    const Spark::Graphics::IDenoiser* GetDenoiser() const { return m_denoiser.get(); }

    /**
     * @brief Get the current denoiser backend (Software / OIDN / OptiX / etc).
     *
     * Returns `DenoiserBackend::None` on a headless build where the
     * denoiser was never constructed, or the concrete backend the
     * current denoiser reports.
     */
    Spark::Graphics::DenoiserBackend GetDenoiserBackend() const;

    // ========================================================================
    // Phase S: Procedural noise accessor
    // ========================================================================

    /**
     * @brief Get the engine's procedural noise graph (Phase S activation).
     *
     * The `GraphicsEngine` owns one `NoiseGraph` instance, default-
     * initialised with a single `SimplexNode` as the output. Terrain,
     * foliage scatter, and any other procedural system can call
     * `Evaluate(x, y)` / `BatchEvaluate(...)` through this accessor
     * without creating a standalone graph or touching a singleton.
     *
     * The graph exposes the full node catalog (Perlin, Cellular, FBM,
     * DomainWarp, Combiner) via `AddNode(std::make_unique<...>)` for
     * callers that want to compose their own graphs; the default
     * output is a Simplex node so the accessor is useful out of the
     * box. Runtime SIMD detection is available via
     * `GetProceduralNoiseSIMDLevel()`.
     */
    Spark::Graphics::NoiseGraph* GetProceduralNoise() { return m_proceduralNoise.get(); }
    const Spark::Graphics::NoiseGraph* GetProceduralNoise() const { return m_proceduralNoise.get(); }

    /**
     * @brief Get the runtime-detected SIMD level used by the noise graph.
     *
     * Returns `Scalar` on non-x86 platforms or when neither SSE2 nor
     * AVX2 is available. The level is detected at compile time from
     * the preprocessor feature macros, not at runtime — swap the
     * compiler flags to change it.
     */
    Spark::Graphics::SIMDLevel GetProceduralNoiseSIMDLevel() const;

    // ========================================================================
    // Phase T: Voxel cone traced GI accessor
    // ========================================================================

    /**
     * @brief Get the voxel cone traced GI system (Phase T activation).
     *
     * The `GraphicsEngine` owns one `VCTSystem` instance, default-
     * initialised with a small 32³ voxel grid (~130 KB) and
     * `enabled = false` so no per-frame voxelization or cone-trace
     * work runs until a future GI render pass opts in. Callers
     * that want full-resolution GI can re-initialise the system
     * with `VCTSettings{.voxelResolution = 128, .worldExtent = 100.0f}`
     * which reallocates the grid at ~9 MB.
     *
     * The VCT pipeline is four stages:
     * 1. `BeginVoxelization()` — clear the grid.
     * 2. `InjectLight(...)` / `InjectGeometry(...)` — rasterise the
     *    scene into the voxel grid.
     * 3. `EndVoxelization()` — build the mip chain for cone filtering.
     * 4. `TraceDiffuse(...)` / `TraceSpecular(...)` — gather indirect
     *    lighting per surface point.
     *
     * All four stages run on pure CPU and work on every platform.
     */
    Spark::Graphics::VCTSystem* GetVCTSystem() { return m_vctSystem.get(); }
    const Spark::Graphics::VCTSystem* GetVCTSystem() const { return m_vctSystem.get(); }

    /** @brief Set non-owning physics pointer (called by engine during init) */
    void SetPhysicsSystem(PhysicsSystem* physics) { m_physicsSystem = physics; }

    LightManager* GetLightManager() const;

    // ========================================================================
    // ADVANCED RENDERING FEATURES
    // ========================================================================

    /**
     * @brief Set the active rendering pipeline
     * @param pipeline Rendering pipeline type
     */
    void SetRenderingPipeline(RenderingPipeline pipeline);

    /**
     * @brief Get the current rendering pipeline
     */
    RenderingPipeline GetRenderingPipeline() const;

    /**
     * @brief Set graphics settings
     */
    void SetGraphicsSettings(const GraphicsSettings& settings);
    const GraphicsSettings& GetGraphicsSettings() const;

    /**
     * @brief Set quality preset
     */
    void SetQualityPreset(QualityPreset preset);

    /**
     * @brief Set render path
     */
    void SetRenderPath(RenderPath path);

    /**
     * @brief Enable/disable HDR rendering
     * @param enabled HDR state
     */
    void SetHDREnabled(bool enabled);

    /**
     * @brief Set multi-sampling anti-aliasing level
     * @param msaaLevel MSAA level
     */
    void SetMSAALevel(MSAALevel msaaLevel);

    /**
     * @brief Configure temporal anti-aliasing
     * @param settings TAA settings
     */
    void SetTAASettings(const TAASettings& settings);

    /**
     * @brief Configure screen-space ambient occlusion
     * @param settings SSAO settings
     */
    void SetSSAOSettings(const SSAOSettings& settings);

    /**
     * @brief Configure screen-space reflections
     * @param settings SSR settings
     */
    void SetSSRSettings(const SSRSettings& settings);

    /**
     * @brief Configure volumetric lighting
     * @param settings Volumetric lighting settings
     */
    void SetVolumetricSettings(const VolumetricSettings& settings);

    // ========================================================================
    // RESOURCE ACCESS
    // ========================================================================

    /**
     * @brief Get the DirectX 11 device
     */
    ID3D11Device* GetDevice() const;

    /**
     * @brief Get the DirectX 11 device context
     */
    ID3D11DeviceContext* GetContext() const;

    /**
     * @brief Get the current window width
     */
    UINT GetWindowWidth() const;

    /**
     * @brief Get the current window height
     */
    UINT GetWindowHeight() const;

    /**
     * @brief Get the DXGI swap chain
     */
    IDXGISwapChain* GetSwapChain() const;

    /**
     * @brief Get render target views
     */
    ID3D11RenderTargetView* GetBackBufferRTV() const;
    ID3D11DepthStencilView* GetDepthStencilView() const;

    // ========================================================================
    // STATISTICS AND PERFORMANCE
    // ========================================================================

    /**
     * @brief Get render statistics
     */
    const RenderStatistics& GetStatistics() const;
    void ResetStatistics();

    /**
     * @brief Save screenshot
     */
    HRESULT SaveScreenshot(const std::string& filename);

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // Console commands are registered via GraphicsConsoleCommands.h.
    // These methods implement the underlying operations.
    // ========================================================================

    RenderStatistics Console_GetStatistics() const;
    void Console_SetQuality(const std::string& preset);
    void Console_SetRenderPath(const std::string& path);
    void Console_EnableFeature(const std::string& feature, bool enabled);
    void Console_SetSetting(const std::string& setting, float value);
    void Console_ReloadShaders();
    bool Console_Screenshot(const std::string& filename);
    std::string Console_GetSystemInfo() const;
    std::string Console_Benchmark(int seconds = 10);
    void Console_SetWireframe(bool enabled);
    void Console_SetVSync(bool enabled);
    void Console_SetRenderingPipeline(RenderingPipeline pipeline);
    void Console_SetHDR(bool enabled);
    void Console_SetDebugMode(bool enabled);
    void Console_SetClearColor(float r, float g, float b, float a);
    void Console_SetRenderScale(float scale);
    void Console_ResetDevice();
    void Console_ForceGarbageCollection();
    void Console_ApplySettings(const GraphicsSettings& settings);
    void Console_ResetToDefaults();
    void Console_RegisterStateCallback(std::function<void()> callback);
    void Console_SetGPUTiming(bool enabled);
    size_t Console_GetVRAMUsage() const;

#ifdef SPARK_HYBRID_RT
    /// @brief Get the hybrid RT manager for console command access
    Spark::Graphics::HybridRTManager* GetHybridRT() { return m_hybridRT.get(); }
#endif

    // ========================================================================
    // BASIC SHADER SYSTEM (for rendering pipeline)
    // ========================================================================

    /**
     * @brief Set basic shaders for rendering
     */
    void SetBasicShaders();

    /**
     * @brief Update basic constant buffer with transformation matrices
     * @param world World transformation matrix
     * @param view View transformation matrix  
     * @param proj Projection transformation matrix
     */
    void UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                              const DirectX::XMMATRIX& proj);

    /**
     * @brief Update basic constant buffer with per-object color and UV tiling
     * @param world World transformation matrix
     * @param view View transformation matrix
     * @param proj Projection transformation matrix
     * @param color Object color multiplier (multiplied with the bound texture)
     * @param uvTiling UV tiling factors (x,y) for the bound texture
     */
    void UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                              const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT4& color,
                              const DirectX::XMFLOAT2& uvTiling);

    /**
     * @brief Basic constants with emissive glow, per-frame alpha, and a UV offset.
     * @param emissive  MaterialProperties.z — surface color added UNLIT (0 = off).
     * @param alpha     MaterialProperties.w — output alpha scale (needs a blend state).
     * @param uvOffset  UVTiling.zw — added after tiling (flipbook frame selection).
     * Emissive/alpha default to off/opaque so this matches the plain overload.
     */
    void UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                              const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT4& color,
                              const DirectX::XMFLOAT2& uvTiling, float emissive, float alpha = 1.0f,
                              const DirectX::XMFLOAT2& uvOffset = DirectX::XMFLOAT2(0.0f, 0.0f));

    /**
     * @brief Blend modes for the basic draw path.
     */
    enum class BasicBlendMode
    {
        Opaque,   ///< default: no blending, depth write on
        Alpha,    ///< src-alpha / inv-src-alpha (holo panels, shields)
        Additive  ///< src + dst (muzzle flashes, energy FX)
    };

    /**
     * @brief Set the output-merger blend state for subsequent basic draws.
     * Remember to restore Opaque after a transparent/additive batch.
     */
    void SetBasicBlendMode(BasicBlendMode mode);

    /**
     * @brief Bind a texture to slot t0 for the basic pixel shader.
     * @param srv Texture SRV; nullptr binds the default 1x1 white texture.
     */
    void SetBasicTexture(ID3D11ShaderResourceView* srv);

    /**
     * @brief Bind the default rasterizer/depth-stencil/blend states used by
     *        the basic-shader draw path (RSSetState / OMSetDepthStencilState /
     *        OMSetBlendState on the current context).
     *
     * Reuses the SAME state objects (m_solidRasterState, m_defaultDepthState,
     * m_defaultBlendState) that BeginFrame()'s ApplyGraphicsState() binds for
     * the windowed runtime path, so callers of Spark::RenderWorldBasic() (the
     * headless test, the editor's attach-mode GraphicsEngine, or a future
     * caller) don't need to set up pipeline state themselves. Safe to call in
     * both windowed and device-attach mode — no-ops on any null state object.
     */
    void ApplyBasicRenderStates();

    /**
     * @brief Simple albedo material resolved from a material JSON for the basic shader path.
     *
     * Loaded lazily from Assets/Materials/... JSON files ("albedo" + "tiling"
     * keys) and cached by path. Cross-DLL safe: pure member state.
     */
    struct BasicMaterial
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; ///< Albedo texture (null = white)
        DirectX::XMFLOAT2 tiling{1.0f, 1.0f};                 ///< UV tiling from the material
    };

    /**
     * @brief Load (or fetch cached) texture SRV from an image file via WIC, with mips.
     * @param path Path to the image file (png/jpg/bmp)
     * @return SRV or nullptr on failure. Cached per path.
     */
    ID3D11ShaderResourceView* GetOrLoadTextureSRV(const std::string& path);

    /**
     * @brief Load (or fetch cached) a BasicMaterial from a material JSON file.
     * @param jsonPath e.g. "Assets/Materials/MMOFPS/Terrain_Rock.json"
     * @return Cached material (srv may be null if textures missing); nullptr on parse failure.
     */
    const BasicMaterial* GetOrLoadBasicMaterial(const std::string& jsonPath);

    /**
     * @brief Update per-frame constants for lighting and camera
     * @param view View transformation matrix
     * @param proj Projection transformation matrix
     * @param cameraPos Camera position in world space
     */
    void UpdateFrameConstants(const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cameraPos);

    /// @brief Get the view matrix from the most recent frame.
    const DirectX::XMMATRIX& GetFrameViewMatrix() const { return m_frameViewMatrix; }

    /// @brief Get the projection matrix from the most recent frame.
    const DirectX::XMMATRIX& GetFrameProjectionMatrix() const { return m_frameProjMatrix; }

    /// @brief Get the camera position from the most recent frame.
    const DirectX::XMFLOAT3& GetFrameCameraPosition() const { return m_frameCameraPos; }

    /// @brief Get near clip plane distance.
    float GetNearPlane() const { return m_nearPlane; }

    /// @brief Get far clip plane distance.
    float GetFarPlane() const { return m_farPlane; }

  private:
    // ========================================================================
    // ADVANCED RENDERING SUBSYSTEMS
    // ========================================================================

    std::unique_ptr<Spark::RHI::RHIBridge> m_rhiBridge; ///< Optional RHI abstraction bridge
    std::unique_ptr<TextureSystem> m_textureSystem;
    std::unique_ptr<MaterialSystem> m_materialSystem;
    std::unique_ptr<LightingSystem> m_lightingSystem;
    std::unique_ptr<AssetPipeline> m_assetPipeline;
    std::unique_ptr<UpscalingSystem> m_upscalingSystem;
    std::unique_ptr<VRAMBudgetMonitor> m_vramBudgetMonitor;
    // Non-owning: PhysicsSystem lifetime managed by SparkEngine.cpp / EngineContext
    PhysicsSystem* m_physicsSystem = nullptr;

    // Shader system
    std::unique_ptr<class Shader> m_shader;

    // Render graph pipeline (DAG-based alternative to hardcoded render paths)
    std::unique_ptr<Spark::Graphics::RenderPipeline> m_renderPipeline;

    // Legacy rendering subsystems
    std::unique_ptr<LightManager> m_lightManager;
    std::unique_ptr<Spark::Graphics::PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<TemporalEffects> m_temporalEffects;
    std::unique_ptr<Spark::Graphics::ShadowAtlas> m_shadowAtlas;
    std::unique_ptr<Spark::Graphics::ScreenSpaceEffects> m_screenSpaceEffects;
    std::unique_ptr<Spark::Graphics::TerrainRenderer> m_terrainRenderer;

#ifdef SPARK_HYBRID_RT
    std::unique_ptr<Spark::Graphics::HybridRTManager> m_hybridRT;
#endif

#ifdef SPARK_HARDWARE_RT
    std::vector<Spark::Graphics::BLASInstance> m_dxrInstances; ///< Per-frame TLAS instance list
#endif

    // ========================================================================
    // DIRECTX RESOURCES
    // ========================================================================

    ComPtr<ID3D11Device1> m_device;
    ComPtr<ID3D11DeviceContext1> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;

    /// True when this GraphicsEngine was set up via InitializeFromDevice()
    /// (attached to a caller-owned device, no swapchain/backbuffer). Guards
    /// swapchain-dependent entry points (BeginFrame/EndFrame/Resize) so they
    /// safely no-op instead of dereferencing a null m_swapChain.
    bool m_attachedMode = false;

    // Advanced render targets for deferred/forward+ rendering
    // G-buffer layout: [0]=Albedo (RGBA8), [1]=Normal (RGB10A2), [2]=Material (RGBA8: roughness/metallic/AO), [3]=Motion vectors (RG16F)
    ComPtr<ID3D11Texture2D> m_gBufferTextures[4];
    ComPtr<ID3D11RenderTargetView> m_gBufferRTVs[4];
    ComPtr<ID3D11ShaderResourceView> m_gBufferSRVs[4];
    ComPtr<ID3D11Texture2D> m_hdrTexture;
    ComPtr<ID3D11RenderTargetView> m_hdrRTV;
    ComPtr<ID3D11ShaderResourceView> m_hdrSRV;
    ComPtr<ID3D11Texture2D> m_depthStencilTexture;
    ComPtr<ID3D11ShaderResourceView> m_depthStencilSRV; ///< Depth SRV for HiZ / post-process reads

    // Enhanced render targets for deferred rendering
    std::unordered_map<std::string, std::unique_ptr<RenderTarget>> m_renderTargets;

    // ========================================================================
    // RENDERING STATE
    // ========================================================================

    RenderingPipeline m_currentPipeline;
    GraphicsSettings m_settings;
    RenderStatistics m_statistics;
    uint32_t m_width;
    uint32_t m_height;
    bool m_fullscreen;
    Spark::NativeWindowHandle m_hwnd;

    // Advanced settings
    bool m_hdrEnabled;
    MSAALevel m_msaaLevel;
    TAASettings m_taaSettings;
    SSAOSettings m_ssaoSettings;
    SSRSettings m_ssrSettings;
    VolumetricSettings m_volumetricSettings;

    // Window dimensions
    UINT m_windowWidth;
    UINT m_windowHeight;

    // ========================================================================
    // PERFORMANCE TRACKING
    // ========================================================================

    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    std::chrono::high_resolution_clock::time_point m_renderStartTime;
    std::chrono::high_resolution_clock::time_point m_geometryStartTime;
    std::chrono::high_resolution_clock::time_point m_lightingStartTime;
    std::chrono::high_resolution_clock::time_point m_postProcessStartTime;

    ComPtr<ID3D11Query> m_disjointQuery;
    ComPtr<ID3D11Query> m_timestampStartQuery;
    ComPtr<ID3D11Query> m_timestampEndQuery;
    ComPtr<ID3D11Query> m_gpuTimingQuery;
    ComPtr<ID3D11Query> m_timestampBegin;
    ComPtr<ID3D11Query> m_timestampEnd;
    ComPtr<ID3D11Query> m_timestampDisjoint;

    // ========================================================================
    // RENDER STATE OBJECTS
    // ========================================================================

    ComPtr<ID3D11RasterizerState> m_defaultRasterState;
    ComPtr<ID3D11RasterizerState> m_wireframeRasterState;
    ComPtr<ID3D11RasterizerState> m_solidRasterState;
    ComPtr<ID3D11DepthStencilState> m_defaultDepthState;
    ComPtr<ID3D11BlendState> m_defaultBlendState;

    // ========================================================================
    // THREADING AND STATE MANAGEMENT
    // ========================================================================

    mutable std::mutex m_metricsMutex;
    std::function<void()> m_stateCallback;

    // Atomic: frame state is queried from multiple threads (render + main loop)
    std::atomic<bool> m_frameInProgress;

    // Resource tracking
    size_t m_textureMemoryUsage;
    size_t m_bufferMemoryUsage;

    // Per-frame ECS draw list populated by SubmitMeshForRendering(), consumed by ProcessDrawList().
    // Uses a pre-reserved vector and a lightweight spinlock instead of std::mutex
    // to minimize contention on the per-entity submission hot path.
    //
    // Double-buffering: ProcessDrawList swaps `m_drawList` with `m_processingDrawList`
    // under the spinlock rather than moving. This preserves the allocated capacity
    // on both vectors frame-to-frame, so steady-state draw submission does zero
    // heap allocations after the first frame's capacity is established.
    std::vector<MeshDrawCommand> m_drawList;
    std::vector<MeshDrawCommand> m_processingDrawList;
    mutable std::atomic_flag m_drawListSpinlock =
        ATOMIC_FLAG_INIT;               ///< Spinlock for draw list (lower overhead than mutex)
    mutable std::mutex m_drawListMutex; ///< Fallback mutex for GetDrawList() copies (cold path only)

    // ========================================================================
    // RENDERER INTEGRATION SYSTEMS
    // ========================================================================

#ifdef SPARK_PLATFORM_WINDOWS
    Spark::Graphics::D3D11PipelineStateCache m_pipelineStateCache; ///< Hash-based D3D11 state caching
    Spark::Graphics::RenderTargetPool m_renderTargetPool;          ///< Pooled transient render targets
    Spark::Graphics::GPUSceneBuffer m_gpuSceneBuffer;              ///< Persistent GPU instance buffer
    Spark::Graphics::BVHAccelerator m_bvhAccelerator;              ///< SAH-based BVH for culling
    Spark::Graphics::ConstantBufferRing m_constantBufferRing;      ///< Ring-buffer CB sub-allocation
    Spark::Graphics::GPUDebugMarkers m_gpuDebugMarkers;            ///< PIX/RenderDoc GPU annotations
    Spark::Graphics::GPUTimestampQuery m_gpuTimestampQuery;        ///< Per-pass GPU timing queries
#endif                                                             // SPARK_PLATFORM_WINDOWS

    // Phase Q: image denoiser (portable — default-constructed as a
    // SoftwareDenoiser joint-bilateral fallback that works on every
    // platform with no external SDK). Owned by unique_ptr so a future
    // swap to OIDN / OptiX is a one-line replacement inside Initialize.
    std::unique_ptr<Spark::Graphics::IDenoiser> m_denoiser;

    // Phase S: procedural noise graph (portable — runtime SIMD
    // detection, default output is a SimplexNode). unique_ptr so a
    // future terrain / foliage scatter system can replace the graph
    // with a domain-specific composition without rebuilding the
    // engine. Owned by unique_ptr because `NoiseGraph` holds a
    // `std::vector<std::unique_ptr<NoiseNode>>` and the raw output
    // pointer points into that vector; moving / copying the graph
    // would invalidate the pointer.
    std::unique_ptr<Spark::Graphics::NoiseGraph> m_proceduralNoise;

    // Phase T: voxel cone traced GI system (portable — CPU voxel
    // grid + mip chain + cone-trace math, no D3D11). unique_ptr
    // so the grid's internal `std::vector<uint8_t>` storage has a
    // stable heap address across any future GraphicsEngine copy,
    // and so a full-resolution GI re-initialisation (32³ → 128³)
    // can be an in-place replace without touching the accessor.
    // Default settings set `enabled = false` + 32³ resolution so
    // the memory footprint stays ~130 KB until a real GI pass
    // opts in.
    std::unique_ptr<Spark::Graphics::VCTSystem> m_vctSystem;
    std::vector<Spark::Graphics::DrawSortEntry> m_sortedDrawList; ///< Sorted draw list per frame
    std::vector<GameObject*> m_culledObjectsBuffer;               ///< Reusable culling output (avoids per-frame alloc)

    // Basic shader system resources (fallback rendering pipeline)
    ComPtr<ID3D11VertexShader> m_basicVertexShader;
    ComPtr<ID3D11PixelShader> m_basicPixelShader;
    ComPtr<ID3D11InputLayout> m_basicInputLayout;
    ComPtr<ID3D11Buffer> m_basicConstantBuffer;
    ComPtr<ID3D11Buffer> m_basicFrameConstantBuffer; ///< Per-frame constant buffer (camera, lighting)
    ComPtr<ID3D11SamplerState> m_basicSamplerState;
    ComPtr<ID3D11Texture2D> m_defaultTexture;      ///< 1x1 white texture used when no material is assigned
    ComPtr<ID3D11ShaderResourceView> m_defaultSRV; ///< SRV for the default white texture
    // Basic-path blend states (lazily created by SetBasicBlendMode).
    ComPtr<ID3D11BlendState> m_blendOpaque;
    ComPtr<ID3D11BlendState> m_blendAlpha;
    ComPtr<ID3D11BlendState> m_blendAdditive;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>>
        m_basicTextureCache;                                             ///< WIC-loaded textures by path
    std::unordered_map<std::string, BasicMaterial> m_basicMaterialCache; ///< Parsed basic materials by JSON path

    // Pre-present hook: invoked in EndFrame() immediately before Present().
    // Plain function pointer (NOT std::function) so a module DLL calling
    // EndFrame() safely invokes exe-side code — used for the game-mode ImGui
    // overlay. Set from the engine executable only.
    using PrePresentHook = void (*)(void* userData);
    PrePresentHook m_prePresentHook = nullptr;
    void* m_prePresentHookUser = nullptr;

  public:
    /// @brief Install a hook called right before SwapChain Present (exe-side overlay rendering).
    void SetPrePresentHook(PrePresentHook hook, void* userData)
    {
        m_prePresentHook = hook;
        m_prePresentHookUser = userData;
    }

  private:
    // ========================================================================
    // PRIVATE METHODS
    // ========================================================================

    // --- Device recovery ---
    bool RecoverFromDeviceLost();                      ///< Attempt to recreate D3D11 device after device-lost event.
    void ReleaseAllDeviceResources();                  ///< Release all COM resources for device recreation.
    uint32_t m_deviceLostRecoveryAttempts = 0;         ///< Number of device-lost recovery attempts
    static constexpr uint32_t MAX_DEVICE_RECOVERY = 3; ///< Max recovery attempts before permanent fallback

    // --- Device and resource creation (called once during Initialize) ---
    HRESULT CreateDeviceAndSwapChain(HWND hWnd); ///< Create D3D11 device, context, and DXGI swap chain.
    HRESULT CreateDevice(HWND hwnd, uint32_t width, uint32_t height, bool fullscreen);
    HRESULT CreateRenderTargetView();      ///< Create the back buffer RTV from the swap chain.
    HRESULT CreateDepthStencilView();      ///< Create the depth/stencil texture and DSV.
    HRESULT CreateRenderTargets();         ///< Create standard render targets (HDR, resolve).
    HRESULT CreateAdvancedRenderTargets(); ///< Create G-buffer textures for deferred rendering.
    HRESULT CreateRenderStates();          ///< Create rasterizer, depth-stencil, and blend states.
    void SetViewport();                    ///< Set the D3D11 viewport to match window dimensions.

    // --- Per-frame state management ---
    void UpdateMetrics();                          ///< Update basic render statistics (draw calls, triangles).
    void UpdateAdvancedMetrics();                  ///< Update GPU timing and memory usage metrics.
    void ApplyGraphicsState();                     ///< Bind rasterizer/depth/blend states based on current settings.
    void ApplyAdvancedGraphicsState();             ///< Configure advanced states (MSAA, HDR tone mapping).
    void ApplyQualityPreset(QualityPreset preset); ///< Set all graphics settings to match a named quality level.
    void NotifyStateChange();                      ///< Fire the state callback (used by editor for UI refresh).
    void SetupDeferredPipeline();                  ///< Configure G-buffer layout and lighting pass for deferred path.
    void SetupForwardPlusPipeline();               ///< Configure light grid and tiled culling for Forward+ path.

    // --- Rendering paths (one is active per frame based on m_currentPipeline) ---
    void RenderForward(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects);
    void RenderDeferred(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                        const std::vector<GameObject*>& objects);
    void RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                           const std::vector<GameObject*>& objects);

    // --- Deferred rendering sub-passes ---
    void FillGBuffer(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void LightingPass(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                     std::vector<GameObject*>& visibleObjects); ///< Frustum cull via BVH, output visible set.
    void RenderGeometryPass();    ///< Draw opaque geometry into G-buffer (Albedo, Normal, Material, Motion).
    void RenderLightingPass();    ///< Full-screen quad resolving G-buffer with accumulated lighting.
    void RenderPostProcessing();  ///< HDR tone mapping, bloom, SSAO, SSR, volumetrics.
    void RenderTemporalEffects(); ///< TAA jitter resolve and motion-vector-based ghosting reduction.

    // Basic shader system methods (fallback pipeline)
    HRESULT InitializeBasicShaders();
    HRESULT CompileShaderFromFile(const std::wstring& filename, const char* entryPoint, const char* shaderModel,
                                  ID3DBlob** blobOut);
    HRESULT CreateBasicConstantBuffer();
    HRESULT CreateDefaultTexture();                          ///< Create 1x1 white fallback texture
    HRESULT CompileEmbeddedVertexShader(ID3DBlob** blobOut); ///< Compile built-in vertex shader from source string
    HRESULT CompileEmbeddedPixelShader(ID3DBlob** blobOut);  ///< Compile built-in pixel shader from source string

    // Per-frame camera state (stored during UpdateFrameConstants for system queries)
    DirectX::XMMATRIX m_frameViewMatrix = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_frameProjMatrix = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 m_frameCameraPos{0.0f, 0.0f, 0.0f};
    float m_nearPlane = 0.1f;
    float m_farPlane = 1000.0f;
};
