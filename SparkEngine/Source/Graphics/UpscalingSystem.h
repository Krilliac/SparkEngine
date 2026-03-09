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

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <algorithm>
#include <cmath>

// =============================================================================
// Upscaling Enums
// =============================================================================

/**
 * @brief Available upscaling modes
 */
enum class UpscalingMode
{
    None, ///< No upscaling, render at native resolution
    FSR1, ///< AMD FidelityFX Super Resolution 1.0 (spatial)
    FSR2, ///< AMD FidelityFX Super Resolution 2.0 (temporal)
    DLSS, ///< NVIDIA Deep Learning Super Sampling
    XeSS  ///< Intel Xe Super Sampling
};

/**
 * @brief Quality presets that determine internal render resolution
 */
enum class UpscalingQuality
{
    UltraPerformance, ///< ~33% render scale (3x upscale)
    Performance,      ///< ~50% render scale (2x upscale)
    Balanced,         ///< ~58% render scale (1.7x upscale)
    Quality,          ///< ~67% render scale (1.5x upscale)
    UltraQuality,     ///< ~77% render scale (1.3x upscale)
    Native            ///< 100% render scale (sharpening only)
};

// =============================================================================
// Input Requirements
// =============================================================================

/**
 * @brief Describes which inputs a given upscaling mode requires
 */
struct UpscalingInputRequirements
{
    bool needsColor = true;          ///< Color buffer (always required)
    bool needsDepth = false;         ///< Depth buffer
    bool needsMotionVectors = false; ///< Per-pixel motion vectors
    bool needsExposure = false;      ///< Auto-exposure value
    bool needsReactiveMask = false;  ///< Reactive/transparency mask
    bool needsJitterOffset = false;  ///< TAA jitter offset for the frame

    /** @brief Get requirements for a specific upscaling mode */
    static UpscalingInputRequirements ForMode(UpscalingMode mode)
    {
        UpscalingInputRequirements req;
        req.needsColor = true;

        switch (mode)
        {
        case UpscalingMode::FSR1:
            // Spatial only - just needs color
            break;

        case UpscalingMode::FSR2:
            req.needsDepth = true;
            req.needsMotionVectors = true;
            req.needsExposure = true;
            req.needsReactiveMask = true;
            req.needsJitterOffset = true;
            break;

        case UpscalingMode::DLSS:
            req.needsDepth = true;
            req.needsMotionVectors = true;
            req.needsExposure = true;
            req.needsJitterOffset = true;
            break;

        case UpscalingMode::XeSS:
            req.needsDepth = true;
            req.needsMotionVectors = true;
            req.needsExposure = true;
            req.needsJitterOffset = true;
            break;

        default:
            break;
        }

        return req;
    }
};

// =============================================================================
// Upscaling Settings
// =============================================================================

/**
 * @brief Configuration for the upscaling system
 */
struct UpscalingSettings
{
    UpscalingMode mode = UpscalingMode::None;
    UpscalingQuality quality = UpscalingQuality::Quality;
    float sharpness = 0.5f; ///< Post-upscale sharpening [0, 1]

    /**
     * @brief Get the render scale factor for a quality preset
     * @return Scale factor in [0, 1] where 1.0 = native resolution
     */
    static float GetRenderScale(UpscalingQuality preset)
    {
        switch (preset)
        {
        case UpscalingQuality::UltraPerformance:
            return 0.33f;
        case UpscalingQuality::Performance:
            return 0.50f;
        case UpscalingQuality::Balanced:
            return 0.58f;
        case UpscalingQuality::Quality:
            return 0.67f;
        case UpscalingQuality::UltraQuality:
            return 0.77f;
        case UpscalingQuality::Native:
            return 1.0f;
        default:
            return 1.0f;
        }
    }

    /**
     * @brief Calculate the render resolution for the given display resolution
     * @param displayWidth  Target display width
     * @param displayHeight Target display height
     * @param outWidth      Computed render width
     * @param outHeight     Computed render height
     */
    void CalculateRenderResolution(uint32_t displayWidth, uint32_t displayHeight, uint32_t& outWidth,
                                   uint32_t& outHeight) const
    {
        if (mode == UpscalingMode::None)
        {
            outWidth = displayWidth;
            outHeight = displayHeight;
            return;
        }

        float scale = GetRenderScale(quality);
        outWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
        outHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));

        // Ensure even dimensions for compute shader dispatch alignment
        outWidth = (outWidth + 1u) & ~1u;
        outHeight = (outHeight + 1u) & ~1u;
    }
};

// =============================================================================
// FSR 1.0 Constants
// =============================================================================

/**
 * @brief Constant buffer for FSR 1.0 EASU (Edge Adaptive Spatial Upsampling) pass
 */
struct alignas(16) FSR1EASUConstants
{
    XMFLOAT4 const0; ///< Input size, output size packed
    XMFLOAT4 const1; ///< Input region offset/size
    XMFLOAT4 const2; ///< Input size reciprocal
    XMFLOAT4 const3; ///< Reserved
};

/**
 * @brief Constant buffer for FSR 1.0 RCAS (Robust Contrast Adaptive Sharpening) pass
 */
struct alignas(16) FSR1RCASConstants
{
    XMFLOAT4 const0; ///< Sharpness attenuation parameter
};

// =============================================================================
// FSR 2.0 Dispatch Description
// =============================================================================

/**
 * @brief Input data for an FSR 2.0 temporal upscaling dispatch
 */
struct FSR2DispatchDescription
{
    ID3D11ShaderResourceView* colorSRV = nullptr;         ///< Low-res color input
    ID3D11ShaderResourceView* depthSRV = nullptr;         ///< Low-res depth
    ID3D11ShaderResourceView* motionVectorsSRV = nullptr; ///< Screen-space motion vectors
    ID3D11ShaderResourceView* exposureSRV = nullptr;      ///< Auto-exposure value
    ID3D11ShaderResourceView* reactiveMaskSRV = nullptr;  ///< Reactive mask for transparency
    ID3D11UnorderedAccessView* outputUAV = nullptr;       ///< Full-res output

    XMFLOAT2 jitterOffset = {0.0f, 0.0f};      ///< Current frame jitter in pixels
    XMFLOAT2 motionVectorScale = {1.0f, 1.0f}; ///< Motion vector scale factor
    float deltaTime = 0.016f;                  ///< Frame delta time in seconds
    float nearPlane = 0.1f;                    ///< Camera near plane
    float farPlane = 1000.0f;                  ///< Camera far plane
    float verticalFOV = 1.0472f;               ///< Vertical FOV in radians (~60 deg)
    bool resetAccumulation = false;            ///< Reset temporal history (on camera cut)
};

// =============================================================================
// DLSS Feature Info
// =============================================================================

/**
 * @brief DLSS feature detection and capability information
 */
struct DLSSFeatureInfo
{
    bool isAvailable = false;     ///< DLSS hardware/driver support detected
    bool isOptimalDriver = false; ///< Running on optimal driver version
    uint32_t minDriverMajor = 0;  ///< Minimum required driver major version
    uint32_t minDriverMinor = 0;  ///< Minimum required driver minor version

    /** @brief Optimal render resolution recommended by DLSS for a given quality */
    struct OptimalResolution
    {
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t minRenderWidth = 0;
        uint32_t minRenderHeight = 0;
        uint32_t maxRenderWidth = 0;
        uint32_t maxRenderHeight = 0;
        float sharpness = 0.0f; ///< Recommended sharpness
    };

    /**
     * @brief Query optimal render resolution for DLSS
     * @param displayWidth  Target display width
     * @param displayHeight Target display height
     * @param quality       DLSS quality mode
     * @return Optimal resolution info
     */
    OptimalResolution QueryOptimalResolution(uint32_t displayWidth, uint32_t displayHeight,
                                             UpscalingQuality quality) const
    {
        OptimalResolution result;
        float scale = UpscalingSettings::GetRenderScale(quality);
        result.renderWidth = static_cast<uint32_t>(static_cast<float>(displayWidth) * scale);
        result.renderHeight = static_cast<uint32_t>(static_cast<float>(displayHeight) * scale);
        result.minRenderWidth = result.renderWidth;
        result.minRenderHeight = result.renderHeight;
        result.maxRenderWidth = displayWidth;
        result.maxRenderHeight = displayHeight;
        result.sharpness = 0.0f;
        return result;
    }
};

// =============================================================================
// XeSS Feature Info
// =============================================================================

/**
 * @brief XeSS feature detection and capability information
 */
struct XeSSFeatureInfo
{
    bool isAvailable = false;        ///< XeSS support detected (works on any GPU via DP4a)
    bool hasXMXAcceleration = false; ///< Intel XMX matrix engine available (Arc GPUs)
    uint32_t driverVersion = 0;      ///< Detected driver version
};

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
     */
    void GetRenderResolution(uint32_t& outWidth, uint32_t& outHeight) const
    {
        outWidth = m_renderWidth;
        outHeight = m_renderHeight;
    }

    /**
     * @brief Get the display resolution (output, after upscaling)
     */
    void GetDisplayResolution(uint32_t& outWidth, uint32_t& outHeight) const
    {
        outWidth = m_displayWidth;
        outHeight = m_displayHeight;
    }

    /**
     * @brief Get the current render scale factor
     */
    float GetCurrentRenderScale() const
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
    void ExecuteFSR1(ID3D11ShaderResourceView* inputColorSRV, ID3D11UnorderedAccessView* outputUAV)
    {
        if (!m_initialized || m_settings.mode != UpscalingMode::FSR1)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !inputColorSRV || !outputUAV)
        {
            return;
        }

        // Pass 1: EASU (upscale)
        {
            FSR1EASUConstants easuConst = {};
            float inputWidth = static_cast<float>(m_renderWidth);
            float inputHeight = static_cast<float>(m_renderHeight);
            float outputWidth = static_cast<float>(m_displayWidth);
            float outputHeight = static_cast<float>(m_displayHeight);

            easuConst.const0 = {inputWidth, inputHeight, outputWidth, outputHeight};
            easuConst.const1 = {0.0f, 0.0f, inputWidth, inputHeight};
            easuConst.const2 = {1.0f / inputWidth, 1.0f / inputHeight, 1.0f / outputWidth, 1.0f / outputHeight};
            easuConst.const3 = {0.0f, 0.0f, 0.0f, 0.0f};

            UpdateFSR1Constants(&easuConst, nullptr);

            // Bind EASU compute shader and dispatch
            // In production: bind m_fsr1EASUShader, input SRV, intermediate UAV
            // context->Dispatch(dispatchX, dispatchY, 1);
        }

        // Pass 2: RCAS (sharpen)
        {
            FSR1RCASConstants rcasConst = {};
            // RCAS attenuation: 0 = maximum sharpening, 2 = no sharpening
            float rcasAttenuation = 2.0f * (1.0f - m_settings.sharpness);
            rcasConst.const0 = {rcasAttenuation, 0.0f, 0.0f, 0.0f};

            UpdateFSR1Constants(nullptr, &rcasConst);

            // Bind RCAS compute shader and dispatch
            // In production: bind m_fsr1RCASShader, intermediate SRV, output UAV
            // context->Dispatch(dispatchX, dispatchY, 1);
        }
#else
        (void)inputColorSRV;
        (void)outputUAV;
#endif
    }

    // ---- FSR 2.0 (Temporal Upscaling) ----

    /**
     * @brief Execute FSR 2.0 temporal upscaling
     *
     * Uses motion vectors, depth, and temporal accumulation for high-quality
     * upscaling. Requires per-frame jitter offsets applied to the projection.
     *
     * @param desc Dispatch description with all required inputs
     */
    void ExecuteFSR2(const FSR2DispatchDescription& desc)
    {
        if (!m_initialized || m_settings.mode != UpscalingMode::FSR2)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context)
        {
            return;
        }

        // FSR 2.0 algorithm outline:
        // 1. Compute luminance stability from current and previous frames
        // 2. Reconstruct previous frame data using motion vectors
        // 3. Compute disocclusion mask (areas with no valid history)
        // 4. Upsample and accumulate using robust temporal filtering
        // 5. Apply reactive mask for transparent/particle objects
        // 6. Lock luminance to prevent flickering
        // 7. Apply optional sharpening (RCAS)
        //
        // In production, this dispatches the FSR 2 compute shaders from
        // the AMD FidelityFX SDK.

        if (desc.resetAccumulation)
        {
            m_fsr2FrameIndex = 0;
        }
        m_fsr2FrameIndex++;
#else
        (void)desc;
#endif
    }

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
                     ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset, bool resetHistory = false)
    {
        if (!m_initialized || m_settings.mode != UpscalingMode::DLSS)
        {
            return;
        }

        if (!m_dlssFeatureInfo.isAvailable)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // DLSS integration outline:
        // 1. Query NVSDK_NGX for optimal settings via NVSDK_NGX_DLSS_GetOptimalSettingsCallback
        // 2. Set DLSS parameters: render size, display size, quality, sharpness
        // 3. Provide input resources: color, depth, motion vectors, exposure
        // 4. Execute DLSS via NVSDK_NGX_D3D11_EvaluateFeature
        // 5. Output is written to the provided UAV
        //
        // Requires the NVIDIA NGX SDK and a compatible RTX/GTX GPU.

        if (resetHistory)
        {
            m_dlssFrameIndex = 0;
        }
        m_dlssFrameIndex++;
#else
        (void)colorSRV;
        (void)depthSRV;
        (void)motionVectorsSRV;
        (void)exposureSRV;
        (void)outputUAV;
        (void)jitterOffset;
        (void)resetHistory;
#endif
    }

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
                     ID3D11UnorderedAccessView* outputUAV, const XMFLOAT2& jitterOffset)
    {
        if (!m_initialized || m_settings.mode != UpscalingMode::XeSS)
        {
            return;
        }

        if (!m_xessFeatureInfo.isAvailable)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // XeSS integration outline:
        // 1. Initialize XeSS context via xessD3D11CreateContext
        // 2. Set quality mode and input/output resolutions
        // 3. Provide input buffers: color, depth, motion vectors, exposure
        // 4. Execute via xessD3D11Execute
        // 5. Works on any GPU via DP4a, accelerated on Intel Arc with XMX
        (void)colorSRV;
        (void)depthSRV;
        (void)motionVectorsSRV;
        (void)exposureSRV;
        (void)outputUAV;
        (void)jitterOffset;
#else
        (void)colorSRV;
        (void)depthSRV;
        (void)motionVectorsSRV;
        (void)exposureSRV;
        (void)outputUAV;
        (void)jitterOffset;
#endif
    }

    /** @brief Get XeSS feature info */
    const XeSSFeatureInfo& GetXeSSFeatureInfo() const { return m_xessFeatureInfo; }

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
    bool IsInitialized() const { return m_initialized; }

    /** @brief Check if any upscaling is active */
    bool IsActive() const { return m_settings.mode != UpscalingMode::None; }

    // ---- Console Integration ----

    /** @brief Get a summary string of upscaling state */
    std::string Console_GetStatus() const
    {
        std::string status = "Upscaling System:\n";

        const char* modeStr = "None";
        switch (m_settings.mode)
        {
        case UpscalingMode::FSR1:
            modeStr = "FSR 1.0";
            break;
        case UpscalingMode::FSR2:
            modeStr = "FSR 2.0";
            break;
        case UpscalingMode::DLSS:
            modeStr = "DLSS";
            break;
        case UpscalingMode::XeSS:
            modeStr = "XeSS";
            break;
        default:
            break;
        }

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

        status += "  Mode: " + std::string(modeStr) + "\n";
        status += "  Quality: " + std::string(qualityStr) + "\n";
        status += "  Sharpness: " + std::to_string(m_settings.sharpness) + "\n";
        status += "  Render: " + std::to_string(m_renderWidth) + "x" + std::to_string(m_renderHeight) + "\n";
        status += "  Display: " + std::to_string(m_displayWidth) + "x" + std::to_string(m_displayHeight) + "\n";
        status += "  Scale: " + std::to_string(static_cast<int>(GetCurrentRenderScale() * 100.0f)) + "%\n";

        // Feature availability
        status += "  DLSS available: " + std::string(m_dlssFeatureInfo.isAvailable ? "YES" : "NO") + "\n";
        status += "  XeSS available: " + std::string(m_xessFeatureInfo.isAvailable ? "YES" : "NO") + "\n";

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

    void DetectFeatures()
    {
        // DLSS detection: query NVIDIA NGX for feature support
        // In production: call NVSDK_NGX_D3D11_Init and check for DLSS capability
        m_dlssFeatureInfo.isAvailable = false;
        m_dlssFeatureInfo.isOptimalDriver = false;

        // XeSS detection: check for DP4a support (available on most modern GPUs)
        // In production: call xessIsOptimal and xessD3D11CreateContext
        m_xessFeatureInfo.isAvailable = false;
        m_xessFeatureInfo.hasXMXAcceleration = false;
    }

    // ---- GPU Resources ----

    bool CreateGPUResources()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
        {
            return false;
        }

        // Create FSR 1.0 constant buffers
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        cbDesc.ByteWidth = sizeof(FSR1EASUConstants);
        HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1EASUConstantBuffer.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }

        cbDesc.ByteWidth = sizeof(FSR1RCASConstants);
        hr = m_device->CreateBuffer(&cbDesc, nullptr, m_fsr1RCASConstantBuffer.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }
#endif
        return true;
    }

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
#endif
    }

    void RecreateUpscalingResources()
    {
        if (!m_initialized)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // Release and recreate intermediate texture at new render resolution
        m_intermediateTexture.Reset();
        m_intermediateSRV.Reset();
        m_intermediateUAV.Reset();

        if (m_settings.mode == UpscalingMode::None || !m_device)
        {
            return;
        }

        // Create intermediate texture for multi-pass upscaling (FSR 1.0 EASU output)
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = m_displayWidth;
        texDesc.Height = m_displayHeight;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        m_device->CreateTexture2D(&texDesc, nullptr, m_intermediateTexture.GetAddressOf());

        if (m_intermediateTexture)
        {
            m_device->CreateShaderResourceView(m_intermediateTexture.Get(), nullptr, m_intermediateSRV.GetAddressOf());
            m_device->CreateUnorderedAccessView(m_intermediateTexture.Get(), nullptr, m_intermediateUAV.GetAddressOf());
        }
#endif
    }

    void UpdateRenderResolution()
    {
        m_settings.CalculateRenderResolution(m_displayWidth, m_displayHeight, m_renderWidth, m_renderHeight);
    }

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
            if (SUCCEEDED(hr))
            {
                memcpy(mapped.pData, easuConst, sizeof(FSR1EASUConstants));
                m_context->Unmap(m_fsr1EASUConstantBuffer.Get(), 0);
            }
        }

        if (rcasConst && m_fsr1RCASConstantBuffer)
        {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_context->Map(m_fsr1RCASConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr))
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

    // Frame counters for temporal modes
    uint32_t m_fsr2FrameIndex = 0;
    uint32_t m_dlssFrameIndex = 0;

    // D3D11 resources (raw pointers are non-owning per engine convention)
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

#ifdef SPARK_PLATFORM_WINDOWS
    // FSR 1.0 resources
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsr1EASUConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsr1RCASConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr1EASUShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_fsr1RCASShader;

    // Intermediate resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_intermediateTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_intermediateSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_intermediateUAV;
#endif
};
