/**
 * @file TemporalEffectsTypes.h
 * @brief Type definitions for temporal effects: enums, settings structs, jitter generation, frame history
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from TemporalEffects.h to keep type definitions separate from
 * the main TemporalEffects class implementation.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

// =============================================================================
// TAA Jitter Sequences
// =============================================================================

/**
 * @brief Jitter pattern types for TAA sub-pixel sampling
 */
enum class JitterPattern
{
    Halton23,           ///< Halton sequence base 2,3 (best quality, default)
    BlueNoise,          ///< Blue noise pattern (low discrepancy)
    Uniform8x,          ///< Regular 8-sample grid
    InterleavedGradient ///< Interleaved gradient noise
};

/**
 * @brief TAA quality presets
 */
enum class TAAQuality
{
    Low,    ///< Fast, minimal ghosting rejection
    Medium, ///< Balanced quality/performance
    High,   ///< Full neighborhood clamping + variance clip
    Ultra   ///< Maximum quality with additional sharpening
};

// =============================================================================
// TAA Settings
// =============================================================================

/**
 * @brief Configuration for Temporal Anti-Aliasing
 */
struct TAASettings
{
    bool enabled = true;
    TAAQuality quality = TAAQuality::High;
    JitterPattern jitterPattern = JitterPattern::Halton23;
    int jitterSequenceLength = 16; ///< Samples before repeating

    float historyBlendFactor = 0.9f; ///< Blend with history [0=current only, 1=history only]
    float varianceClipGamma = 1.0f;  ///< Variance clipping aggressiveness
    bool useMotionVectors = true;    ///< Use per-pixel motion vectors for reprojection
    bool useYCoCg = true;            ///< Color space conversion for better clamping
    float sharpness = 0.0f;          ///< Post-TAA sharpening [0, 1]

    // Anti-ghosting
    float ghostingRejectionStrength = 0.8f; ///< How aggressively to reject ghosting [0, 1]
    float flickerReduction = 0.5f;          ///< Flicker suppression strength [0, 1]
};

// =============================================================================
// Motion Blur Settings
// =============================================================================

/**
 * @brief Motion blur technique types
 */
enum class MotionBlurType
{
    CameraOnly, ///< Only camera movement causes blur
    PerObject,  ///< Per-object motion vectors for blur
    Combined    ///< Both camera and per-object blur
};

/**
 * @brief Configuration for motion blur
 */
struct MotionBlurSettings
{
    bool enabled = false;
    MotionBlurType type = MotionBlurType::Combined;
    float intensity = 0.5f;            ///< Blur strength [0, 1]
    int sampleCount = 8;               ///< Blur samples (higher = smoother)
    float maxBlurRadius = 32.0f;       ///< Maximum blur in pixels
    float velocityScale = 1.0f;        ///< Velocity multiplier
    float minVelocityThreshold = 0.5f; ///< Minimum velocity to trigger blur (pixels)

    // Camera blur
    float cameraRotationScale = 0.5f;    ///< Blur from camera rotation
    float cameraTranslationScale = 1.0f; ///< Blur from camera movement

    // Tile-based optimization
    int tileSize = 20; ///< Tile size for velocity tiling
};

// =============================================================================
// Jitter Generation
// =============================================================================

/**
 * @brief Generates sub-pixel jitter offsets for TAA
 */
namespace JitterGenerator
{

    /**
     * @brief Halton sequence value for a given index and base
     */
    inline float HaltonSequence(int index, int base)
    {
        float result = 0.0f;
        float f = 1.0f / static_cast<float>(base);
        int i = index;
        while (i > 0)
        {
            result += f * (i % base);
            i /= base;
            f /= static_cast<float>(base);
        }
        return result;
    }

    /**
     * @brief Generate a jitter offset for the given frame index
     *
     * Returns an offset in [-0.5, 0.5] pixel range that should be
     * applied to the projection matrix for sub-pixel sampling.
     *
     * @param frameIndex  Current frame number
     * @param pattern     Jitter pattern to use
     * @param sequenceLen Number of samples before repeating
     * @return            (x, y) offset in pixel space
     */
    inline XMFLOAT2 GetJitterOffset(int frameIndex, JitterPattern pattern, int sequenceLen)
    {
        int idx = (frameIndex % sequenceLen) + 1; // 1-based for Halton

        switch (pattern)
        {
        case JitterPattern::Halton23:
            return {HaltonSequence(idx, 2) - 0.5f, HaltonSequence(idx, 3) - 0.5f};

        case JitterPattern::Uniform8x:
        {
            // 8-sample rotated grid
            static const XMFLOAT2 offsets[8] = {{-0.375f, -0.375f}, {0.125f, -0.375f}, {-0.125f, -0.125f},
                                                {0.375f, -0.125f},  {-0.375f, 0.125f}, {0.125f, 0.125f},
                                                {-0.125f, 0.375f},  {0.375f, 0.375f}};
            return offsets[idx % 8];
        }

        case JitterPattern::InterleavedGradient:
        {
            float x = static_cast<float>(idx);
            float jx = std::fmod(52.9829189f * std::fmod(0.06711056f * x + 0.00583715f * x, 1.0f), 1.0f) - 0.5f;
            float jy = std::fmod(52.9829189f * std::fmod(0.00583715f * x + 0.06711056f * x * 1.7f, 1.0f), 1.0f) - 0.5f;
            return {jx, jy};
        }

        default:
            return {HaltonSequence(idx, 2) - 0.5f, HaltonSequence(idx, 3) - 0.5f};
        }
    }
} // namespace JitterGenerator

// =============================================================================
// Motion Vector Data
// =============================================================================

/**
 * @brief Per-pixel motion vector for temporal reprojection
 */
struct MotionVectorData
{
    float velocityX = 0.0f; ///< Horizontal velocity in pixels
    float velocityY = 0.0f; ///< Vertical velocity in pixels
    float depth = 1.0f;     ///< Depth for depth-aware blending
};

// =============================================================================
// Frame History
// =============================================================================

/**
 * @class FrameHistory
 * @brief Manages history buffers for temporal accumulation
 *
 * Stores previous frame data needed for TAA reprojection and
 * motion blur velocity calculations.
 */
class FrameHistory
{
  public:
    static constexpr int MAX_HISTORY_FRAMES = 4;

    struct FrameData
    {
        XMFLOAT4X4 viewMatrix;
        XMFLOAT4X4 projMatrix;
        XMFLOAT4X4 viewProjMatrix;
        XMFLOAT4X4 invViewProjMatrix;
        XMFLOAT2 jitterOffset = {0.0f, 0.0f};
        uint32_t frameIndex = 0;
        float deltaTime = 0.0f;
    };

    /**
     * @brief Push a new frame's data into history
     */
    void PushFrame(const FrameData& data)
    {
        // Shift history
        for (int i = MAX_HISTORY_FRAMES - 1; i > 0; --i)
        {
            m_frames[i] = m_frames[i - 1];
        }
        m_frames[0] = data;
        if (m_frameCount < MAX_HISTORY_FRAMES)
            m_frameCount++;
    }

    /** @brief Get the current frame (index 0) */
    const FrameData& GetCurrentFrame() const { return m_frames[0]; }

    /** @brief Get a previous frame (1 = last frame, 2 = two frames ago, etc.) */
    const FrameData& GetPreviousFrame(int framesBack = 1) const
    {
        int idx = std::clamp(framesBack, 0, m_frameCount - 1);
        return m_frames[idx];
    }

    /** @brief Get the number of stored history frames */
    int GetFrameCount() const { return m_frameCount; }

    /** @brief Check if enough history exists for temporal effects */
    bool HasSufficientHistory() const { return m_frameCount >= 2; }

    /** @brief Clear all stored history */
    void Clear()
    {
        m_frameCount = 0;
        for (auto& f : m_frames)
            f = {};
    }

  private:
    std::array<FrameData, MAX_HISTORY_FRAMES> m_frames;
    int m_frameCount = 0;
};

// =============================================================================
// Neighborhood Clamping (TAA anti-ghosting)
// =============================================================================

/**
 * @brief Color clamping utilities for TAA ghosting rejection
 */
namespace NeighborhoodClamp
{

    /** @brief RGB color for clamping operations */
    struct ColorRGB
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;

        ColorRGB operator+(const ColorRGB& o) const { return {r + o.r, g + o.g, b + o.b}; }
        ColorRGB operator-(const ColorRGB& o) const { return {r - o.r, g - o.g, b - o.b}; }
        ColorRGB operator*(float s) const { return {r * s, g * s, b * s}; }

        static ColorRGB Min(const ColorRGB& a, const ColorRGB& b)
        {
            return {std::min(a.r, b.r), std::min(a.g, b.g), std::min(a.b, b.b)};
        }
        static ColorRGB Max(const ColorRGB& a, const ColorRGB& b)
        {
            return {std::max(a.r, b.r), std::max(a.g, b.g), std::max(a.b, b.b)};
        }
    };

    /**
     * @brief Convert RGB to YCoCg color space for better clamping
     */
    inline ColorRGB RGBToYCoCg(const ColorRGB& rgb)
    {
        return {
            0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b, // Y
            0.5f * rgb.r - 0.5f * rgb.b,                  // Co
            -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b // Cg
        };
    }

    /**
     * @brief Convert YCoCg back to RGB
     */
    inline ColorRGB YCoCgToRGB(const ColorRGB& ycocg)
    {
        float y = ycocg.r;
        float co = ycocg.g;
        float cg = ycocg.b;
        return {y + co - cg, y + cg, y - co - cg};
    }

    /**
     * @brief Clamp a history color to the AABB of a neighborhood
     *
     * @param historyColor  The reprojected history sample
     * @param neighborMin   Minimum color in the 3x3 neighborhood
     * @param neighborMax   Maximum color in the 3x3 neighborhood
     * @return              Clamped color
     */
    inline ColorRGB ClampToAABB(const ColorRGB& historyColor, const ColorRGB& neighborMin, const ColorRGB& neighborMax)
    {
        return {std::clamp(historyColor.r, neighborMin.r, neighborMax.r),
                std::clamp(historyColor.g, neighborMin.g, neighborMax.g),
                std::clamp(historyColor.b, neighborMin.b, neighborMax.b)};
    }

    /**
     * @brief Variance-based clip (more accurate than AABB clamp)
     *
     * Clips history color toward neighborhood mean using variance,
     * controlled by gamma factor.
     *
     * @param historyColor   The reprojected history sample
     * @param neighborMean   Mean color of 3x3 neighborhood
     * @param neighborStdDev Standard deviation of 3x3 neighborhood
     * @param gamma          Clipping aggressiveness (1.0 = standard, higher = looser)
     */
    inline ColorRGB VarianceClip(const ColorRGB& historyColor, const ColorRGB& neighborMean,
                                 const ColorRGB& neighborStdDev, float gamma = 1.0f)
    {
        ColorRGB minBound = neighborMean - neighborStdDev * gamma;
        ColorRGB maxBound = neighborMean + neighborStdDev * gamma;
        return ClampToAABB(historyColor, minBound, maxBound);
    }
} // namespace NeighborhoodClamp
