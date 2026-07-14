/**
 * @file PostProcessingPipeline.h
 * @brief Configurable post-processing effect chain with ordered passes
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a flexible pipeline for chaining post-processing effects in a
 * configurable order. Each effect is a self-contained pass with its own
 * settings, enable/disable state, and performance metrics. The pipeline
 * manages render target ping-ponging between passes.
 *
 * ## Usage
 * @code
 *   PostProcessingPipeline pipeline;
 *   pipeline.Initialize(1920, 1080);
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::Vignette, true);
 *   pipeline.GetVignetteSettings().intensity = 0.4f;
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::DepthOfField, true);
 *   pipeline.GetDOFSettings().focalDistance = 15.0f;
 *
 *   // Each frame after scene rendering
 *   pipeline.Process(deltaTime);
 * @endcode
 */

#pragma once

// Subsystem headers
#include "PostProcessingEffects.h"
// SSAOTemporalFilter is a CPU-side reference implementation and is
// unconditionally available (no D3D11 dependency) — the pipeline owns one
// instance for per-frame history state even on Linux / headless builds.
#include "SSAOTemporal.h"
// VolumeManager is Phase K's activation — portable CPU code that spatially
// blends post-process settings based on camera position. Owned by the
// pipeline so every call to Process() can apply the live volume stack
// into the pass settings structs.
#include "VolumeSystem.h"
// RTHandleSystem is Phase N's portable activation — Unity-HDRP-style
// scale-based render texture handles whose allocation tracks the
// reference viewport without reallocating on shrink. Ticked from the
// pipeline's own Initialize / Resize paths.
#include "RTHandleSystem.h"

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

// Phase J: Tier 2 graphics orphan activation — these utilities are all
// Windows-only (they wrap D3D11 interfaces) and gated on the same guard.
#include "GPUDebugMarkers.h"
#include "GPUTimestampQuery.h"
#include "RenderTargetPool.h"
// Phase N: ConstantBufferRing is another Windows-only D3D11 wrapper —
// Dynamic sub-allocation for per-frame constant buffer updates.
#include "ConstantBufferRing.h"
#endif

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    // =============================================================================
    // Post-Processing Pipeline
    // =============================================================================

    /**
     * @class PostProcessingPipeline
     * @brief Manages an ordered chain of post-processing effects
     *
     * Effects are processed in a fixed order (the PostProcessPass enum order).
     * Each effect can be independently enabled/disabled. The pipeline handles
     * render target ping-ponging automatically.
     */
    class PostProcessingPipeline
    {
      public:
        PostProcessingPipeline() = default;
        ~PostProcessingPipeline() = default;

        /** @brief Initialize the pipeline with render dimensions */
        bool Initialize(uint32_t width = 1920, uint32_t height = 1080);

        /** @brief Shutdown and release all resources */
        void Shutdown();

        /** @brief Check whether Initialize has run successfully */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Process all enabled effects in order
         * @param deltaTime Frame delta time for animated effects
         */
        void Process(float deltaTime = 0.0f);

        /** @brief Render the final result to the currently bound render target */
        void Render();

        /** @brief Handle viewport resize, recreating GPU targets if needed */
        void Resize(uint32_t width, uint32_t height);

        // ---- Effect Enable/Disable ----

        void SetEffectEnabled(PostProcessPass pass, bool enabled) { m_passEnabled[static_cast<int>(pass)] = enabled; }

        bool IsEffectEnabled(PostProcessPass pass) const { return m_passEnabled[static_cast<int>(pass)]; }

        int GetActivePassCount() const { return m_activePassCount; }

        // ---- Settings Accessors ----

        FXAASettings& GetFXAASettings() { return m_fxaaSettings; }
        const FXAASettings& GetFXAASettings() const { return m_fxaaSettings; }

        DepthOfFieldSettings& GetDOFSettings() { return m_dofSettings; }
        const DepthOfFieldSettings& GetDOFSettings() const { return m_dofSettings; }

        VignetteSettings& GetVignetteSettings() { return m_vignetteSettings; }
        const VignetteSettings& GetVignetteSettings() const { return m_vignetteSettings; }

        ChromaticAberrationSettings& GetChromaticAberrationSettings() { return m_chromAbSettings; }
        const ChromaticAberrationSettings& GetChromaticAberrationSettings() const { return m_chromAbSettings; }

        FilmGrainSettings& GetFilmGrainSettings() { return m_filmGrainSettings; }
        const FilmGrainSettings& GetFilmGrainSettings() const { return m_filmGrainSettings; }

        LensDistortionSettings& GetLensDistortionSettings() { return m_lensDistSettings; }
        const LensDistortionSettings& GetLensDistortionSettings() const { return m_lensDistSettings; }

        LightShaftSettings& GetLightShaftSettings() { return m_lightShaftSettings; }
        const LightShaftSettings& GetLightShaftSettings() const { return m_lightShaftSettings; }

        LensFlareSettings& GetLensFlareSettings() { return m_lensFlareSettings; }
        const LensFlareSettings& GetLensFlareSettings() const { return m_lensFlareSettings; }

        SharpenSettings& GetSharpenSettings() { return m_sharpenSettings; }
        const SharpenSettings& GetSharpenSettings() const { return m_sharpenSettings; }

        BloomSettings& GetBloomSettings() { return m_bloomSettings; }
        const BloomSettings& GetBloomSettings() const { return m_bloomSettings; }

        GTAOSettings& GetGTAOSettings() { return m_gtaoSettings; }
        const GTAOSettings& GetGTAOSettings() const { return m_gtaoSettings; }

        SSAOTemporalSettings& GetSSAOTemporalSettings() { return m_ssaoTemporalFilter.GetSettings(); }
        const SSAOTemporalSettings& GetSSAOTemporalSettings() const { return m_ssaoTemporalFilter.GetSettings(); }

        SSAOTemporalFilter& GetSSAOTemporalFilter() { return m_ssaoTemporalFilter; }
        const SSAOTemporalFilter& GetSSAOTemporalFilter() const { return m_ssaoTemporalFilter; }

        // ---- Phase K: Volume manager accessors ----

        /**
         * @brief Get the spatial post-process volume manager.
         *
         * The manager is lifecycle-owned by the pipeline and updated once
         * per call to `Process()`. Callers can add global or local volumes
         * with `CreateVolume()`, attach parameter components to them, and
         * set the camera position via `SetCameraPosition()` — on the next
         * frame the blended `VolumeStack` is applied to the pipeline's
         * effect settings.
         */
        VolumeManager& GetVolumeManager() { return m_volumeManager; }
        const VolumeManager& GetVolumeManager() const { return m_volumeManager; }

        /**
         * @brief Set the camera world position used by the volume manager.
         *
         * This is the only signal the volume blend needs — on the next
         * `Process()` call, all local volumes compute their AABB-distance
         * blend factor against this position. Global volumes ignore it.
         * Default position is the origin.
         */
        void SetCameraPosition(const XMFLOAT3& position) { m_cameraPosition = position; }
        const XMFLOAT3& GetCameraPosition() const { return m_cameraPosition; }

        /**
         * @brief Whether `Process()` should push the blended volume stack
         *        into the effect settings structs each frame.
         *
         * Defaults to `true`. Disable it if you want `VolumeManager` to
         * collect a stack for queries without mutating the live pipeline
         * settings — useful for preview/debug panels.
         */
        void SetVolumeBlendEnabled(bool enabled) { m_volumeBlendEnabled = enabled; }
        bool IsVolumeBlendEnabled() const { return m_volumeBlendEnabled; }

        /**
         * @brief Apply the current `VolumeStack` to the effect settings.
         *
         * Public so tests can exercise the blend without calling `Process()`.
         * The stack's four component types map onto the pipeline settings as:
         *   - Exposure      → `m_autoExposureSettings` (compensationEV)
         *   - Bloom         → `m_bloomSettings`        (intensity, threshold, softThreshold, scatter)
         *   - ColorGrading  → `m_colorGradingSettings` (lift, gain, saturation, contrast, temperature, tint)
         *   - Fog           → unused (no fog in the pipeline — left for a future fog pass)
         */
        void ApplyVolumeStack();

        AutoExposureSettings& GetAutoExposureSettings() { return m_autoExposureSettings; }
        const AutoExposureSettings& GetAutoExposureSettings() const { return m_autoExposureSettings; }

        TonemappingSettings& GetTonemappingSettings() { return m_tonemappingSettings; }
        const TonemappingSettings& GetTonemappingSettings() const { return m_tonemappingSettings; }

        ColorGradingSettings& GetColorGradingSettings() { return m_colorGradingSettings; }
        const ColorGradingSettings& GetColorGradingSettings() const { return m_colorGradingSettings; }

        // ---- Metrics ----

        std::vector<PassMetrics> GetPassMetrics() const;
        std::string Console_ListEffects() const;

        // ---- Phase J: Tier 2 orphan activation surface ----

        /**
         * @brief Number of render targets owned by the per-frame RT pool.
         *
         * Windows builds return the live count from `RenderTargetPool::GetMetrics`;
         * non-Windows builds always return 0 (the pool is Windows-only).
         */
        uint32_t GetRenderTargetPoolSize() const;

        /**
         * @brief Console-friendly render-target pool status.
         *
         * On Windows returns the pool's own status line; on other platforms
         * returns a short "(not compiled on this platform)" marker so UI
         * panels that display this never get an empty string.
         */
        std::string Console_RenderTargetPoolStatus() const;

        /**
         * @brief Current GPU debug marker nesting depth (for PIX/RenderDoc).
         *
         * Returns 0 on non-Windows builds. Used by tests to assert that
         * BeginPass/Render never leak an unbalanced event region.
         */
        uint32_t GetGPUMarkerDepth() const;

        /**
         * @brief Most recent GPU-side time in milliseconds for a pass.
         *
         * Returns the `GPUTimestampQuery` reading for the pass name when a
         * D3D11 device is attached; otherwise returns the CPU-side
         * `m_passTimings` value so callers always get a non-negative number.
         */
        float GetPassTimeMs(PostProcessPass pass) const;

        // ---- Phase N: RTHandleSystem (portable) ----

        /**
         * @brief Get the render-target handle system.
         *
         * The pipeline owns one `RTHandleSystem` instance sized to the
         * pipeline's reference viewport. Callers allocate scale-based
         * handles (e.g. a half-resolution bloom target at
         * `scaleX = scaleY = 0.5f`), and the system tracks per-handle
         * allocation bookkeeping without reallocating on shrink.
         *
         * `Resize()` forwards the new reference size via
         * `RTHandleSystem::SetReferenceSize` so every allocated handle
         * grows / updates its current dimensions automatically.
         */
        RTHandleSystem& GetRTHandleSystem() { return m_rtHandleSystem; }
        const RTHandleSystem& GetRTHandleSystem() const { return m_rtHandleSystem; }

        // ---- Phase N: ConstantBufferRing (Windows-only) ----

        /**
         * @brief Current ring-buffer capacity in bytes (0 on non-Windows).
         *
         * The `ConstantBufferRing` is only allocated on Windows when a
         * D3D11 device is attached. Callers can use this to decide
         * whether to sub-allocate from the ring or fall back to the
         * per-draw `Map` / `Unmap` path.
         */
        uint32_t GetConstantBufferRingCapacity() const;

        /**
         * @brief Peak constant-buffer-ring usage in bytes across all frames.
         *
         * Zero on non-Windows. Reset by `Shutdown()`.
         */
        uint32_t GetConstantBufferRingPeakUsage() const;

        void Console_SetExposure(float value) { m_lightShaftSettings.exposure = value; }

        /** @brief Set the D3D11 device and context for GPU execution */
        void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context);

        /** @brief Set the scene depth SRV for depth-aware effects */
        void SetDepthSRV(ID3D11ShaderResourceView* depthSRV) { m_depthSRV = depthSRV; }

        /** @brief Set the input scene texture SRV */
        void SetInputSRV(ID3D11ShaderResourceView* inputSRV) { m_inputSRV = inputSRV; }

        /** @brief Get the output render target view after processing */
        ID3D11RenderTargetView* GetOutputRTV() const { return m_pingPongRTVs[m_currentTarget]; }

        /** @brief Get the output SRV after processing */
        ID3D11ShaderResourceView* GetOutputSRV() const { return m_pingPongSRVs[m_currentTarget]; }

      private:
        // ---- GPU Constant Buffer ----
        struct PostProcessCB
        {
            XMFLOAT4 params0;
            XMFLOAT4 params1;
            XMFLOAT4 params2;
            XMFLOAT4 params3;
        };

        // ---- GPU Resource Creation ----
        bool CreatePingPongTargets();
        bool CreatePostProcessShaders();
        void CompileEffectShaders();

        // ---- Pass Execution ----
        void SwapTargets() { m_currentTarget = 1 - m_currentTarget; }
        int GetSourceTarget() const { return 1 - m_currentTarget; }
        void BeginPass(ID3D11PixelShader* ps, const PostProcessCB& cb);
        void DrawFullscreen();
        void ProcessPass(PostProcessPass pass, float deltaTime);

        // ---- Pipeline state ----
        bool m_initialized = false;
        uint32_t m_width = 1920;
        uint32_t m_height = 1080;
        float m_totalTime = 0.0f;
        int m_activePassCount = 0;
        int m_currentTarget = 0;
        bool m_vsAlreadyBound = false; ///< Avoids redundant VS/sampler/topology binding between passes

        bool m_passEnabled[static_cast<int>(PostProcessPass::Count)] = {};
        float m_passTimings[static_cast<int>(PostProcessPass::Count)] = {};

        // Per-effect settings
        BloomSettings m_bloomSettings;
        GTAOSettings m_gtaoSettings;
        // SSAOTemporalFilter is an all-CPU orphan, so it is owned unconditionally.
        // Its `Initialize(width, height)` gets called from the pipeline's own
        // Initialize and Resize paths so the history buffer follows viewport size.
        SSAOTemporalFilter m_ssaoTemporalFilter;

        // Phase K: VolumeManager is pure CPU code and runs on every platform.
        // Initialised alongside the pipeline; Update() runs once per Process()
        // call and ApplyVolumeStack() pushes the blended stack into the
        // existing effect settings structs.
        VolumeManager m_volumeManager;
        XMFLOAT3 m_cameraPosition = {0.0f, 0.0f, 0.0f};
        bool m_volumeBlendEnabled = true;

        // Phase N: RTHandleSystem is pure CPU code (the allocation metadata
        // layer, not the GPU texture layer), so it lives outside the
        // Windows guard. Initialized from Initialize(width, height) and
        // SetReferenceSize()-ed from Resize().
        RTHandleSystem m_rtHandleSystem;
        AutoExposureSettings m_autoExposureSettings;
        TonemappingSettings m_tonemappingSettings;
        ColorGradingSettings m_colorGradingSettings;
        FXAASettings m_fxaaSettings;
        DepthOfFieldSettings m_dofSettings;
        VignetteSettings m_vignetteSettings;
        ChromaticAberrationSettings m_chromAbSettings;
        FilmGrainSettings m_filmGrainSettings;
        LensDistortionSettings m_lensDistSettings;
        LightShaftSettings m_lightShaftSettings;
        LensFlareSettings m_lensFlareSettings;
        SharpenSettings m_sharpenSettings;

        // Auto-exposure state (persists between frames)
        float m_currentExposure = 1.0f;

        // GPU resources
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        ID3D11ShaderResourceView* m_depthSRV = nullptr;
        ID3D11ShaderResourceView* m_inputSRV = nullptr;

        // Ping-pong render targets
        ComPtr<ID3D11Texture2D> m_pingPongTextures[2];
        ID3D11RenderTargetView* m_pingPongRTVs[2] = {};
        ID3D11ShaderResourceView* m_pingPongSRVs[2] = {};

        // Per-pass pixel shaders
#ifdef SPARK_PLATFORM_WINDOWS
        // Phase J orphan activations. All three wrap D3D11 interfaces and
        // are therefore guarded on the same platform toggle as the rest of
        // the render state above.
        RenderTargetPool m_rtPool;    ///< Per-device transient RT allocator (Phase J)
        GPUDebugMarkers m_gpuMarkers; ///< PIX / RenderDoc scoped event regions (Phase J)
        GPUTimestampQuery m_gpuTimer; ///< Per-pass GPU timestamp queries (Phase J)
        // Phase N: ring-buffer CB allocator (Windows-only — wraps
        // ID3D11Buffer directly). Lifecycle follows the D3D11 device.
        ConstantBufferRing m_cbRing;
#endif
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_bloomPS;
        ComPtr<ID3D11PixelShader> m_gtaoPS;
        ComPtr<ID3D11PixelShader> m_ssaoTemporalPS;
        ComPtr<ID3D11PixelShader> m_autoExposurePS;
        ComPtr<ID3D11PixelShader> m_tonemapPS;
        ComPtr<ID3D11PixelShader> m_colorGradingPS;
        ComPtr<ID3D11PixelShader> m_fxaaPS;
        ComPtr<ID3D11PixelShader> m_dofPS;
        ComPtr<ID3D11PixelShader> m_motionBlurPS;
        ComPtr<ID3D11PixelShader> m_vignettePS;
        ComPtr<ID3D11PixelShader> m_chromAbPS;
        ComPtr<ID3D11PixelShader> m_filmGrainPS;
        ComPtr<ID3D11PixelShader> m_lensDistPS;
        ComPtr<ID3D11PixelShader> m_lightShaftsPS;
        ComPtr<ID3D11PixelShader> m_lensFlarePS;
        ComPtr<ID3D11PixelShader> m_sharpenPS;

        // Shared GPU resources
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11SamplerState> m_linearSampler;
        ComPtr<ID3D11SamplerState> m_pointSampler;
    };

} // namespace Spark::Graphics
