/**
 * @file UpscalingSystem.h
 * @brief Upscaling integration: FSR 1.0/2.0, DLSS, XeSS
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a unified interface for spatial and temporal upscaling techniques
 * including AMD FSR 1.0 (spatial), FSR 2.0 (temporal), NVIDIA DLSS, and
 * Intel XeSS. Manages render resolution calculation, GPU resources, and
 * per-mode input requirements.
 *
 * @note VENDOR SDK INTEGRATION STATUS (March 2026):
 * The upscaling framework (resolution scaling, quality presets, input routing)
 * is fully implemented, but vendor SDK integration is NOT yet complete:
 *   - NVIDIA DLSS: Requires DLSS SDK (nvngx_dlss.dll) — not linked
 *   - AMD FSR 2.0+: Requires FidelityFX SDK — not linked
 *   - Intel XeSS: Requires XeSS SDK (libxess.dll) — not linked
 *   - FSR 1.0 (spatial): Uses a built-in shader approximation (functional)
 * The Execute() path currently uses a temporal upsampling shader as fallback
 * for all modes except FSR 1.0. To enable vendor upscaling, integrate the
 * respective SDK and implement the vendor-specific dispatch in Execute().
 *
 * ## Usage
 * @code
 *   UpscalingSystem upscaling;
 *   upscaling.Initialize(device, context, 1920, 1080);
 *
 *   UpscalingSettings settings;
 *   settings.mode = UpscalingMode::FSR1;
 *   settings.quality = UpscalingQuality::Quality;
 *   upscaling.SetSettings(settings);
 *
 *   auto renderRes = upscaling.GetRenderResolution();
 *   // ... render scene at renderRes ...
 *   upscaling.Execute(colorSRV, outputUAV);
 * @endcode
 */

#pragma once

#include "DynamicQualityTypes.h"

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <array>
#include <string>
#include <utility>
#include <vector>

// =============================================================================
// Upscaling System
// =============================================================================

/**
 * @class UpscalingSystem
 * @brief Unified upscaling system supporting FSR 1.0/2.0, DLSS, and XeSS
 *
 * Manages render resolution calculation, GPU compute shader resources,
 * and dispatches upscaling passes. Provides a consistent interface
 * regardless of the active upscaling backend.
 */
class UpscalingSystem
{
  public:
    UpscalingSystem() = default;
    ~UpscalingSystem() { Shutdown(); }

    // ---- Lifecycle ----

    /**
     * @brief Initialize the upscaling system
     * @param device        D3D11 device
     * @param context       D3D11 device context
     * @param displayWidth  Target display resolution width
     * @param displayHeight Target display resolution height
     * @return true on success
     */
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t displayWidth = 1920,
                    uint32_t displayHeight = 1080)
    {
        if (m_initialized)
        {
            return false;
        }

        m_device = device;
        m_context = context;
        m_displayWidth = displayWidth;
        m_displayHeight = displayHeight;

        DetectFeatures();

        if (!CreateGPUResources())
        {
            return false;
        }

        m_initialized = true;
        UpdateRenderResolution();
        return true;
    }

    /**
     * @brief Shutdown and release all resources
     */
    void Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        ReleaseGPUResources();
        m_initialized = false;
    }

    // ---- Render Resolution ----

    /**
     * @brief Get the current render resolution (internal, before upscaling)
     * @return Render resolution (use structured bindings: auto [w, h] = ...)
     */
    Resolution GetRenderResolution() const noexcept { return {m_renderWidth, m_renderHeight}; }

    /**
     * @brief Get the display resolution (output, after upscaling)
     * @return Display resolution (use structured bindings: auto [w, h] = ...)
     */
    Resolution GetDisplayResolution() const noexcept { return {m_displayWidth, m_displayHeight}; }

    /**
     * @brief Get the current render scale factor
     */
    float GetCurrentRenderScale() const noexcept
    {
        if (m_settings.mode == UpscalingMode::None)
        {
            return 1.0f;
        }
        return UpscalingSettings::GetRenderScale(m_settings.quality);
    }

    /**
     * @brief Handle display resolution change
     */
    void OnDisplayResize(uint32_t newWidth, uint32_t newHeight)
    {
        m_displayWidth = newWidth;
        m_displayHeight = newHeight;
        UpdateRenderResolution();
        RecreateUpscalingResources();
    }

    // ---- FSR 1.0 (Spatial Upscaling) ----

    /**
     * @brief Execute FSR 1.0 spatial upscaling (EASU + RCAS)
     *
     * Two-pass compute shader approach:
     *   1. EASU: Edge-Adaptive Spatial Upsampling (lanczos-like with edge detection)
     *   2. RCAS: Robust Contrast-Adaptive Sharpening
     *
     * @param inputColorSRV  Low-resolution color input
     * @param outputUAV      Full-resolution output
     */
    void ExecuteFSR1(ID3D11ShaderResourceView* inputColorSRV, ID3D11UnorderedAccessView* outputUAV);

    // ---- FSR 2.0 (Temporal Upscaling) ----

    /**
     * @brief Execute FSR 2.0 temporal upscaling
     *
     * Uses motion vectors, depth, and temporal accumulation for high-quality
     * upscaling. Requires per-frame jitter offsets applied to the projection.
     *
     * @param desc Dispatch description with all required inputs
     */
    void ExecuteFSR2(const FSR2DispatchDescription& desc);

    // ---- DLSS ----

    /**
     * @brief Execute DLSS upscaling
     *
     * @param colorSRV         Low-res color input
     * @param depthSRV         Low-res depth buffer
     * @param motionVectorsSRV Screen-space motion vectors
     * @param exposureSRV      Auto-exposure value (optional)
     * @param outputUAV        Full-res output
     * @param jitterOffset     Current frame jitter in pixels
     * @param resetHistory     Reset temporal accumulation
     */
    void ExecuteDLSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                     ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                     ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset, bool resetHistory = false);

    /** @brief Get DLSS feature info */
    const DLSSFeatureInfo& GetDLSSFeatureInfo() const { return m_dlssFeatureInfo; }

    // ---- XeSS ----

    /**
     * @brief Execute XeSS upscaling
     *
     * @param colorSRV         Low-res color input
     * @param depthSRV         Low-res depth buffer
     * @param motionVectorsSRV Screen-space motion vectors
     * @param exposureSRV      Auto-exposure value (optional)
     * @param outputUAV        Full-res output
     * @param jitterOffset     Current frame jitter in pixels
     */
    void ExecuteXeSS(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                     ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                     ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset);

    /** @brief Get XeSS feature info */
    const XeSSFeatureInfo& GetXeSSFeatureInfo() const { return m_xessFeatureInfo; }

    // ---- SparkSR (Engine-Native Temporal Upscaling) ----

    /**
     * @brief Execute SparkSR temporal upscaling
     *
     * SparkEngine's own vendor-independent temporal upscaler using YCoCg
     * variance clipping, motion confidence weighting, and improved disocclusion
     * detection. Two-pass pipeline: temporal accumulation + RCAS sharpening.
     * No vendor SDK required — works on any GPU with compute shader support.
     *
     * @param colorSRV         Low-res color input
     * @param depthSRV         Low-res depth buffer
     * @param motionVectorsSRV Screen-space motion vectors
     * @param exposureSRV      Auto-exposure value (optional)
     * @param reactiveMaskSRV  Reactive mask for particles/transparency (optional)
     * @param outputUAV        Full-res output
     * @param jitterOffset     Current frame jitter in pixels
     * @param resetHistory     Reset temporal accumulation (camera cut)
     */
    void ExecuteSparkSR(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                        ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                        ID3D11ShaderResourceView* reactiveMaskSRV, ID3D11UnorderedAccessView* outputUAV,
                        const XMFLOAT2& jitterOffset, bool resetHistory = false);

    /** @brief Check if SparkSR is available (always true — shader-based, no vendor SDK) */
    bool IsSparkSRAvailable() const noexcept { return m_shadersCompiled; }

    // ---- Unified Execute ----

    /**
     * @brief Execute the currently active upscaling mode
     *
     * Routes to the appropriate backend based on m_settings.mode.
     * For FSR1, only colorSRV and outputUAV are used.
     * For temporal modes, all inputs should be provided.
     *
     * @param colorSRV         Low-res color input
     * @param depthSRV         Low-res depth (temporal modes)
     * @param motionVectorsSRV Motion vectors (temporal modes)
     * @param exposureSRV      Exposure value (temporal modes)
     * @param outputUAV        Full-res output
     * @param jitterOffset     Jitter offset (temporal modes)
     */
    void Execute(ID3D11ShaderResourceView* colorSRV, ID3D11ShaderResourceView* depthSRV,
                 ID3D11ShaderResourceView* motionVectorsSRV, ID3D11ShaderResourceView* exposureSRV,
                 ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset = {0.0f, 0.0f})
    {
        switch (m_settings.mode)
        {
        case UpscalingMode::FSR1:
            ExecuteFSR1(colorSRV, outputUAV);
            break;

        case UpscalingMode::FSR2:
        {
            FSR2DispatchDescription desc;
            desc.colorSRV = colorSRV;
            desc.depthSRV = depthSRV;
            desc.motionVectorsSRV = motionVectorsSRV;
            desc.exposureSRV = exposureSRV;
            desc.outputUAV = outputUAV;
            desc.jitterOffset = jitterOffset;
            ExecuteFSR2(desc);
            break;
        }

        case UpscalingMode::DLSS:
            ExecuteDLSS(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, outputUAV, jitterOffset);
            break;

        case UpscalingMode::XeSS:
            ExecuteXeSS(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, outputUAV, jitterOffset);
            break;

        case UpscalingMode::SparkSR:
            ExecuteSparkSR(colorSRV, depthSRV, motionVectorsSRV, exposureSRV, nullptr, outputUAV, jitterOffset);
            break;

        default:
            break;
        }
    }

    // ---- Input Requirements ----

    /**
     * @brief Get the input requirements for the current upscaling mode
     */
    UpscalingInputRequirements GetCurrentInputRequirements() const
    {
        return UpscalingInputRequirements::ForMode(m_settings.mode);
    }

    // ---- Settings ----

    UpscalingSettings& GetSettings() { return m_settings; }
    const UpscalingSettings& GetSettings() const { return m_settings; }

    void SetSettings(const UpscalingSettings& settings)
    {
        bool modeChanged = (settings.mode != m_settings.mode);
        bool qualityChanged = (settings.quality != m_settings.quality);
        m_settings = settings;

        if (modeChanged || qualityChanged)
        {
            UpdateRenderResolution();
            RecreateUpscalingResources();
        }
    }

    /** @brief Set the upscaling mode */
    void SetMode(UpscalingMode mode)
    {
        if (mode != m_settings.mode)
        {
            m_settings.mode = mode;
            UpdateRenderResolution();
            RecreateUpscalingResources();
        }
    }

    /** @brief Set the quality preset */
    void SetQuality(UpscalingQuality quality)
    {
        if (quality != m_settings.quality)
        {
            m_settings.quality = quality;
            UpdateRenderResolution();
            RecreateUpscalingResources();
        }
    }

    /** @brief Set the sharpness value */
    void SetSharpness(float sharpness) { m_settings.sharpness = std::clamp(sharpness, 0.0f, 1.0f); }

    /** @brief Check if the system is initialized */
    bool IsInitialized() const noexcept { return m_initialized; }

    /** @brief Check if any upscaling is active */
    bool IsActive() const noexcept { return m_settings.mode != UpscalingMode::None; }

    /**
     * @brief Get the recommended render size for a given display resolution and quality
     * @param displayWidth  Target display width
     * @param displayHeight Target display height
     * @param quality       Quality preset
     * @return Recommended render resolution
     */
    static Resolution GetRecommendedRenderSize(uint32_t displayWidth, uint32_t displayHeight, UpscalingQuality quality);

    /** @brief Check if FSR 1.0 is available (engine-owned EASU/RCAS compute shaders) */
    bool IsFSR1Available() const noexcept { return m_shadersCompiled; }

    /** @brief Check if FSR 2.0 is available (false until the FidelityFX SDK is linked) */
    bool IsFSR2Available() const noexcept { return m_fsr2Available; }

    /** @brief Check if compute shaders were compiled successfully */
    bool AreShadersCompiled() const noexcept { return m_shadersCompiled; }

    // ---- Console Integration ----

    /** @brief Human-readable name of an upscaling mode. */
    static const char* ModeName(UpscalingMode mode) noexcept
    {
        switch (mode)
        {
        case UpscalingMode::FSR1:
            return "FSR 1.0";
        case UpscalingMode::FSR2:
            return "FSR 2.0";
        case UpscalingMode::DLSS:
            return "DLSS";
        case UpscalingMode::XeSS:
            return "XeSS";
        case UpscalingMode::SparkSR:
            return "SparkSR";
        default:
            return "None";
        }
    }

    /**
     * @brief The upscaler Execute() actually runs, which is not always the
     *        requested mode.
     *
     * ExecuteFSR2 / ExecuteDLSS / ExecuteXeSS forward to ExecuteSparkSR whenever the
     * vendor SDK is not linked (see UpscalingSystemWindowsExecute.cpp), so a caller
     * that reads only GetMode() would believe a vendor upscaler is running while
     * SparkSR produces every pixel.
     */
    UpscalingMode GetEffectiveMode() const noexcept
    {
        switch (m_settings.mode)
        {
        case UpscalingMode::FSR2:
            return IsFSR2Available() ? UpscalingMode::FSR2 : UpscalingMode::SparkSR;
        case UpscalingMode::DLSS:
            return m_dlssFeatureInfo.isAvailable ? UpscalingMode::DLSS : UpscalingMode::SparkSR;
        case UpscalingMode::XeSS:
            return m_xessFeatureInfo.isAvailable ? UpscalingMode::XeSS : UpscalingMode::SparkSR;
        default:
            return m_settings.mode;
        }
    }

    /** @brief Get a summary string of upscaling state */
    std::string Console_GetStatus() const
    {
        std::string status = "Upscaling System:\n";

        const char* qualityStr = "Native";
        switch (m_settings.quality)
        {
        case UpscalingQuality::UltraPerformance:
            qualityStr = "Ultra Performance";
            break;
        case UpscalingQuality::Performance:
            qualityStr = "Performance";
            break;
        case UpscalingQuality::Balanced:
            qualityStr = "Balanced";
            break;
        case UpscalingQuality::Quality:
            qualityStr = "Quality";
            break;
        case UpscalingQuality::UltraQuality:
            qualityStr = "Ultra Quality";
            break;
        case UpscalingQuality::Native:
            qualityStr = "Native";
            break;
        }

        // Report the requested mode and, when they differ, the one that actually
        // runs. A status line that says "DLSS" while SparkSR renders every pixel is
        // exactly the reassuring value that hides a missing dependency.
        const UpscalingMode effectiveMode = GetEffectiveMode();
        status += "  Mode: " + std::string(ModeName(m_settings.mode));
        if (effectiveMode != m_settings.mode)
        {
            status += " (running " + std::string(ModeName(effectiveMode)) + " - vendor SDK not linked)";
        }
        status += "\n";
        status += "  Quality: " + std::string(qualityStr) + "\n";
        status += "  Sharpness: " + std::to_string(m_settings.sharpness) + "\n";
        status += "  Render: " + std::to_string(m_renderWidth) + "x" + std::to_string(m_renderHeight) + "\n";
        status += "  Display: " + std::to_string(m_displayWidth) + "x" + std::to_string(m_displayHeight) + "\n";
        status += "  Scale: " + std::to_string(static_cast<int>(GetCurrentRenderScale() * 100.0f)) + "%\n";

        // Feature availability
        status += "  FSR 1.0 available: " + std::string(IsFSR1Available() ? "YES" : "NO") + "\n";
        status += "  FSR 2.0 available: " + std::string(IsFSR2Available() ? "YES" : "NO") + "\n";
        status += "  DLSS available: " + std::string(m_dlssFeatureInfo.isAvailable ? "YES" : "NO") + "\n";
        status += "  XeSS available: " + std::string(m_xessFeatureInfo.isAvailable ? "YES" : "NO") + "\n";
        status += "  SparkSR available: " + std::string(IsSparkSRAvailable() ? "YES" : "NO") + "\n";

        return status;
    }

    /** @brief Console command to set upscaling mode by name */
    void Console_SetMode(const std::string& modeName)
    {
        if (modeName == "none")
        {
            SetMode(UpscalingMode::None);
        }
        else if (modeName == "fsr1")
        {
            SetMode(UpscalingMode::FSR1);
        }
        else if (modeName == "fsr2")
        {
            SetMode(UpscalingMode::FSR2);
        }
        else if (modeName == "dlss")
        {
            SetMode(UpscalingMode::DLSS);
        }
        else if (modeName == "xess")
        {
            SetMode(UpscalingMode::XeSS);
        }
        else if (modeName == "sparksr")
        {
            SetMode(UpscalingMode::SparkSR);
        }
    }

    /** @brief Console command to set quality preset by name */
    void Console_SetQuality(const std::string& qualityName)
    {
        if (qualityName == "ultra_performance")
        {
            SetQuality(UpscalingQuality::UltraPerformance);
        }
        else if (qualityName == "performance")
        {
            SetQuality(UpscalingQuality::Performance);
        }
        else if (qualityName == "balanced")
        {
            SetQuality(UpscalingQuality::Balanced);
        }
        else if (qualityName == "quality")
        {
            SetQuality(UpscalingQuality::Quality);
        }
        else if (qualityName == "ultra_quality")
        {
            SetQuality(UpscalingQuality::UltraQuality);
        }
        else if (qualityName == "native")
        {
            SetQuality(UpscalingQuality::Native);
        }
    }

    /** @brief Console command to set sharpness */
    void Console_SetSharpness(float sharpness) { SetSharpness(sharpness); }

  private:
    // ---- Feature Detection ----

    void DetectFeatures();

    // ---- GPU Resources ----

    bool CreateGPUResources();

    void ReleaseGPUResources()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        m_fsr1EASUConstantBuffer.Reset();
        m_fsr1RCASConstantBuffer.Reset();
        m_fsr1EASUShader.Reset();
        m_fsr1RCASShader.Reset();
        m_intermediateTexture.Reset();
        m_intermediateSRV.Reset();
        m_intermediateUAV.Reset();
        m_sparkSRTemporalCS.Reset();
        m_sparkSRConstantBuffer.Reset();
#endif
    }

    void RecreateUpscalingResources();

    void UpdateRenderResolution()
    {
        auto [w, h] = m_settings.CalculateRenderResolution(m_displayWidth, m_displayHeight);
        m_renderWidth = w;
        m_renderHeight = h;
    }

    /** @brief Compile all upscaling compute shaders from inline HLSL */
    bool CompileUpscalingShaders();

    /** @brief Unbind all compute shader resources */
    void UnbindComputeResources();

    void UpdateFSR1Constants(const FSR1EASUConstants* easuConst, const FSR1RCASConstants* rcasConst)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context)
        {
            return;
        }

        if (easuConst && m_fsr1EASUConstantBuffer)
        {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_context->Map(m_fsr1EASUConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr) && mapped.pData)
            {
                memcpy(mapped.pData, easuConst, sizeof(FSR1EASUConstants));
                m_context->Unmap(m_fsr1EASUConstantBuffer.Get(), 0);
            }
        }

        if (rcasConst && m_fsr1RCASConstantBuffer)
        {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_context->Map(m_fsr1RCASConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr) && mapped.pData)
            {
                memcpy(mapped.pData, rcasConst, sizeof(FSR1RCASConstants));
                m_context->Unmap(m_fsr1RCASConstantBuffer.Get(), 0);
            }
        }
#else
        (void)easuConst;
        (void)rcasConst;
#endif
    }

    // ---- Member Variables ----

    bool m_initialized = false;

    // Resolution
    uint32_t m_displayWidth = 1920;
    uint32_t m_displayHeight = 1080;
    uint32_t m_renderWidth = 1920;
    uint32_t m_renderHeight = 1080;

    // Settings
    UpscalingSettings m_settings;

    // Feature info
    DLSSFeatureInfo m_dlssFeatureInfo;
    XeSSFeatureInfo m_xessFeatureInfo;

    // Feature availability flags
    bool m_fsr2Available = false;
    bool m_shadersCompiled = false;

    // Frame counters for temporal modes
    uint32_t m_fsr2FrameIndex = 0;
    uint32_t m_dlssFrameIndex = 0;
    uint32_t m_xessFrameIndex = 0;
    uint32_t m_sparkSRFrameIndex = 0;

    // SparkSR jitter tracking for delta computation
    float m_prevJitterX = 0.0f;
    float m_prevJitterY = 0.0f;

    // D3D11 resources (raw pointers are non-owning per engine convention)
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

#ifdef SPARK_PLATFORM_WINDOWS
    // FSR 1.0 resources
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsr1EASUConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsr1RCASConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr1EASUShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr1RCASShader;

    // FSR 2.0 / temporal resources
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr2TemporalCS;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr2SharpenCS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsr2ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_temporalUpscaleConstantBuffer;

    // Temporal history textures (for FSR2 accumulation)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_temporalHistoryTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_temporalHistorySRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_temporalHistoryUAV;

    // Intermediate resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_intermediateTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_intermediateSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_intermediateUAV;

    // Lock texture for luminance locking (FSR2)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_lockTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_lockSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_lockUAV;

    // SparkSR resources
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_sparkSRTemporalCS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sparkSRConstantBuffer;

    // Sampler state for upscaling shaders
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClampSampler;
#endif
};
