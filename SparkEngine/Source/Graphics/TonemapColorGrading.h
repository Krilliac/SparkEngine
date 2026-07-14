/**
 * @file TonemapColorGrading.h
 * @brief Tonemapping, auto-exposure, and color grading post-processing
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides three tightly-coupled post-processing stages:
 * 1. Auto-Exposure: Histogram-based luminance → exposure value
 * 2. Tonemapping:   HDR→LDR conversion (ACES, Filmic, Reinhard, Neutral)
 * 3. Color Grading: LGG, saturation, contrast, white balance, color curves
 *
 * These are combined because they share the same constant buffer and execute
 * sequentially in a single fullscreen pass for efficiency.
 *
 * @see PostProcessingPipeline.h, VolumeSystem.h, BloomEffect.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    // =========================================================================
    // Tonemapping
    // =========================================================================

    enum class TonemapOperator : uint8_t
    {
        None,     ///< Linear (no tonemapping)
        Reinhard, ///< Simple Reinhard (L / (1+L))
        ACES,     ///< Academy Color Encoding System fitted curve
        Filmic,   ///< Uncharted 2 / Hable filmic curve
        Neutral,  ///< Khronos PBR Neutral tone mapper
        Count
    };

    struct TonemapSettings
    {
        bool enabled = true;
        TonemapOperator op = TonemapOperator::ACES;
        float exposure = 1.0f; ///< Manual exposure multiplier (pre-tonemap)
    };

    // =========================================================================
    // Auto-Exposure
    // =========================================================================

    enum class ExposureMode : uint8_t
    {
        Fixed,     ///< Manual exposure control
        Automatic, ///< Histogram-based auto-exposure
        Count
    };

    struct AutoExposureSettings
    {
        bool enabled = false;
        ExposureMode mode = ExposureMode::Automatic;
        float minEV = -2.0f;              ///< Minimum exposure value (EV100)
        float maxEV = 14.0f;              ///< Maximum exposure value (EV100)
        float adaptationSpeedUp = 3.0f;   ///< Speed of brightening adaptation
        float adaptationSpeedDown = 1.0f; ///< Speed of darkening adaptation
        float compensationEV = 0.0f;      ///< Exposure compensation bias
        float targetMidGray = 0.18f;      ///< Target middle gray luminance
    };

    // =========================================================================
    // Color Grading
    // =========================================================================

    struct ColorGradingSettings
    {
        bool enabled = false;

        // Lift / Gamma / Gain (shadows / midtones / highlights)
        XMFLOAT3 lift = {0.0f, 0.0f, 0.0f};  ///< Shadow color offset [-1, 1]
        XMFLOAT3 gamma = {1.0f, 1.0f, 1.0f}; ///< Midtone gamma [0.01, 5]
        XMFLOAT3 gain = {1.0f, 1.0f, 1.0f};  ///< Highlight multiplier [0, 5]

        // Global adjustments
        float saturation = 1.0f;  ///< Color saturation [0=grayscale, 2=vivid]
        float contrast = 1.0f;    ///< Contrast around mid-gray [0.5, 2]
        float temperature = 0.0f; ///< White balance temperature shift [-1=cool, 1=warm]
        float tint = 0.0f;        ///< White balance tint [-1=green, 1=magenta]
    };

    // =========================================================================
    // Combined Tonemap + Color Grading Effect
    // =========================================================================

    /**
     * @brief Combined auto-exposure, tonemapping, and color grading pass
     *
     * Executes in a single fullscreen draw:
     * 1. Compute average luminance (via downsampled luminance reduction)
     * 2. Adapt exposure over time
     * 3. Apply exposure, tonemap, then color grade
     */
    class TonemapColorGrading
    {
      public:
        TonemapColorGrading() = default;
        ~TonemapColorGrading() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);
        void Shutdown();
        void Resize(uint32_t width, uint32_t height);

        /**
         * @brief Execute tonemap + color grading on the input HDR texture
         * @param inputSRV     HDR scene texture (possibly with bloom composited)
         * @param bloomSRV     Bloom texture to add (can be nullptr)
         * @param outputRTV    Render target to write LDR result
         * @param deltaTime    Frame time for exposure adaptation
         */
        void Execute(ID3D11ShaderResourceView* inputSRV, ID3D11ShaderResourceView* bloomSRV,
                     ID3D11RenderTargetView* outputRTV, float deltaTime);

        TonemapSettings& GetTonemapSettings() { return m_tonemapSettings; }
        const TonemapSettings& GetTonemapSettings() const { return m_tonemapSettings; }

        AutoExposureSettings& GetAutoExposureSettings() { return m_autoExposureSettings; }
        const AutoExposureSettings& GetAutoExposureSettings() const { return m_autoExposureSettings; }

        ColorGradingSettings& GetColorGradingSettings() { return m_colorGradingSettings; }
        const ColorGradingSettings& GetColorGradingSettings() const { return m_colorGradingSettings; }

        /** @brief Get the current adapted exposure value */
        float GetCurrentExposure() const { return m_currentExposure; }

        float GetLastGpuTimeMs() const { return m_lastGpuTimeMs; }

      private:
        bool CompileShaders();
        bool CreateResources();
        void ComputeAverageLuminance(ID3D11ShaderResourceView* inputSRV);
        void AdaptExposure(float deltaTime);

        // GPU constant buffer layout
        struct TonemapCB
        {
            XMFLOAT4 exposureParams; // x=exposure, y=tonemapOp, z=bloomIntensity, w=unused
            XMFLOAT4 colorLift;      // rgb=lift, w=saturation
            XMFLOAT4 colorGamma;     // rgb=1/gamma, w=contrast
            XMFLOAT4 colorGain;      // rgb=gain, w=unused
            XMFLOAT4 whiteBalance;   // x=temperature, y=tint, z=unused, w=unused
            XMFLOAT4 texelSize;      // x=1/w, y=1/h, z=unused, w=unused
        };

        TonemapSettings m_tonemapSettings;
        AutoExposureSettings m_autoExposureSettings;
        ColorGradingSettings m_colorGradingSettings;

        // Device
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;

        // Luminance reduction chain (for auto-exposure)
        static constexpr int kLumMips = 7; // 128→64→32→16→8→4→2→1
        ComPtr<ID3D11Texture2D> m_lumTextures[kLumMips];
        ComPtr<ID3D11RenderTargetView> m_lumRTVs[kLumMips];
        ComPtr<ID3D11ShaderResourceView> m_lumSRVs[kLumMips];

        // Adapted luminance (double-buffered for temporal smoothing)
        ComPtr<ID3D11Texture2D> m_adaptedLum[2];
        ComPtr<ID3D11RenderTargetView> m_adaptedLumRTV[2];
        ComPtr<ID3D11ShaderResourceView> m_adaptedLumSRV[2];
        int m_currentLumBuffer = 0;

        // Shaders
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_luminancePS;     // Extract log luminance
        ComPtr<ID3D11PixelShader> m_lumDownsamplePS; // Average downsample
        ComPtr<ID3D11PixelShader> m_adaptExposurePS; // Temporal adaptation
        ComPtr<ID3D11PixelShader> m_tonemapPS;       // Final tonemap + color grade

        // Resources
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11SamplerState> m_linearClampSampler;
        ComPtr<ID3D11SamplerState> m_pointSampler;

        float m_currentExposure = 1.0f;
        float m_lastGpuTimeMs = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
