/**
 * @file DynamicQualityTypes.h
 * @brief Type definitions for the upscaling/dynamic quality system
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enums, structs, and configuration types used by UpscalingSystem.
 * Extracted from UpscalingSystem.h to reduce header size and allow lightweight
 * inclusion of upscaling types without pulling in the full system class.
 */

#pragma once

#include "../Core/Platform.h"

// DirectXMath types provided by Platform.h (stubs on Linux).
// d3d11.h types provided by Platform.h (stubs on Linux).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

/**
 * @brief Resolution as a width/height pair, enabling structured bindings
 */
struct Resolution
{
    uint32_t width = 0;
    uint32_t height = 0;
};

// =============================================================================
// Upscaling Enums
// =============================================================================

/**
 * @brief Available upscaling modes
 */
enum class UpscalingMode
{
    None,   ///< No upscaling, render at native resolution
    FSR1,   ///< AMD FidelityFX Super Resolution 1.0 (spatial)
    FSR2,   ///< AMD FidelityFX Super Resolution 2.0 (temporal)
    DLSS,   ///< NVIDIA Deep Learning Super Sampling
    XeSS,   ///< Intel Xe Super Sampling
    SparkSR ///< SparkEngine native temporal upscaling (no vendor SDK required)
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

        case UpscalingMode::SparkSR:
            req.needsDepth = true;
            req.needsMotionVectors = true;
            req.needsExposure = true;
            req.needsReactiveMask = true;
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
    float sharpness = 0.5f;         ///< Post-upscale sharpening [0, 1]
    bool neuralEnhancement = false; ///< Apply neural SR refinement pass after upscaling

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
     * @return Computed render resolution (use structured bindings: auto [w, h] = ...)
     */
    Resolution CalculateRenderResolution(uint32_t displayWidth, uint32_t displayHeight) const
    {
        if (mode == UpscalingMode::None)
        {
            return {displayWidth, displayHeight};
        }

        float scale = GetRenderScale(quality);
        uint32_t w = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayWidth) * scale));
        uint32_t h = std::max(1u, static_cast<uint32_t>(static_cast<float>(displayHeight) * scale));

        // Ensure even dimensions for compute shader dispatch alignment
        w = (w + 1u) & ~1u;
        h = (h + 1u) & ~1u;
        return {w, h};
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
// FSR 2.0 Constants
// =============================================================================

/**
 * @brief Constant buffer for FSR 2.0 temporal upscaling pass
 */
struct alignas(16) FSR2Constants
{
    XMFLOAT4 renderSize;     ///< (renderW, renderH, 1/renderW, 1/renderH)
    XMFLOAT4 displaySize;    ///< (displayW, displayH, 1/displayW, 1/displayH)
    XMFLOAT4 jitterOffset;   ///< (jitterX, jitterY, motionScaleX, motionScaleY)
    XMFLOAT4 cameraParams;   ///< (nearPlane, farPlane, verticalFOV, deltaTime)
    XMFLOAT4 temporalParams; ///< (frameIndex, resetFlag, sharpness, reserved)
};

// =============================================================================
// DLSS/XeSS Constants
// =============================================================================

/**
 * @brief Constant buffer shared by DLSS/XeSS fallback path
 */
struct alignas(16) TemporalUpscaleConstants
{
    XMFLOAT4 renderSize;     ///< (renderW, renderH, 1/renderW, 1/renderH)
    XMFLOAT4 displaySize;    ///< (displayW, displayH, 1/displayW, 1/displayH)
    XMFLOAT4 jitterOffset;   ///< (jitterX, jitterY, 0, 0)
    XMFLOAT4 temporalParams; ///< (frameIndex, resetFlag, sharpness, reserved)
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
// SparkSR Constants
// =============================================================================

/**
 * @brief Constant buffer for SparkSR native temporal upscaling
 *
 * Enhanced temporal upscaler with YCoCg variance clipping, motion confidence,
 * and improved disocclusion detection. No vendor SDK required.
 */
struct alignas(16) SparkSRConstants
{
    XMFLOAT4 renderSize;     ///< (renderW, renderH, 1/renderW, 1/renderH)
    XMFLOAT4 displaySize;    ///< (displayW, displayH, 1/displayW, 1/displayH)
    XMFLOAT4 jitterOffset;   ///< (jitterX, jitterY, prevJitterX, prevJitterY)
    XMFLOAT4 temporalParams; ///< (frameIndex, resetFlag, sharpness, blendMin)
    XMFLOAT4 motionParams;   ///< (motionScaleX, motionScaleY, depthThreshold, varianceGamma)
};
