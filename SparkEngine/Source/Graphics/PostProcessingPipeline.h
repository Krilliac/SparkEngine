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

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
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

        /**
         * @brief Process all enabled effects in order
         * @param deltaTime Frame delta time for animated effects
         */
        void Process(float deltaTime = 0.0f);

        /** @brief Render the final result to the currently bound render target */
        void Render();

        /** @brief Handle viewport resize, recreating GPU targets if needed */
        void Resize(uint32_t width, uint32_t height)
        {
            if (width == 0 || height == 0)
            {
                return;
            }

            if (m_width == width && m_height == height)
            {
                return;
            }

            m_width = width;
            m_height = height;

            // Recreate ping-pong targets at new resolution
            if (m_initialized && m_device)
            {
                CreatePingPongTargets();
            }
        }

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

        AutoExposureSettings& GetAutoExposureSettings() { return m_autoExposureSettings; }
        const AutoExposureSettings& GetAutoExposureSettings() const { return m_autoExposureSettings; }

        TonemappingSettings& GetTonemappingSettings() { return m_tonemappingSettings; }
        const TonemappingSettings& GetTonemappingSettings() const { return m_tonemappingSettings; }

        ColorGradingSettings& GetColorGradingSettings() { return m_colorGradingSettings; }
        const ColorGradingSettings& GetColorGradingSettings() const { return m_colorGradingSettings; }

        // ---- Metrics ----

        std::vector<PassMetrics> GetPassMetrics() const;
        std::string Console_ListEffects() const;

        void Console_SetExposure(float value) { m_lightShaftSettings.exposure = value; }

        /** @brief Set the D3D11 device and context for GPU execution */
        void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            m_device = device;
            m_context = context;
        }

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
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_bloomPS;
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
