/**
 * @file TemporalEffects.h
 * @brief Temporal effects system: TAA, motion blur, temporal reprojection
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides frame-to-frame temporal rendering techniques including Temporal
 * Anti-Aliasing (TAA), per-object and camera motion blur, and temporal
 * reprojection utilities for stable rendering.
 *
 * ## Usage
 * @code
 *   TemporalEffects temporal;
 *   temporal.Initialize(width, height);
 *   temporal.SetTAAEnabled(true);
 *   temporal.SetMotionBlurEnabled(true);
 *
 *   // Each frame
 *   temporal.BeginFrame(viewMatrix, projMatrix, deltaTime);
 *   // ... render scene ...
 *   temporal.EndFrame();
 *
 *   auto jitter = temporal.GetJitterOffset(); // Apply to projection matrix
 *   float blendFactor = temporal.GetHistoryBlendFactor();
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>

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

// =============================================================================
// Temporal Effects System
// =============================================================================

/**
 * @class TemporalEffects
 * @brief Manages TAA, motion blur, and frame history for temporal rendering
 *
 * Coordinates temporal anti-aliasing jitter, history buffer management,
 * motion vector calculation, and motion blur rendering. Integrates with
 * the rendering pipeline by providing jitter offsets and blend factors.
 */
class TemporalEffects
{
  public:
    TemporalEffects() = default;
    ~TemporalEffects() = default;

    /**
     * @brief Initialize temporal effects with render target dimensions
     * @param width  Render target width in pixels
     * @param height Render target height in pixels
     * @return true on success
     */
    bool Initialize(uint32_t width = 1920, uint32_t height = 1080)
    {
        m_width = width;
        m_height = height;
        m_frameHistory.Clear();
        m_frameIndex = 0;
        m_initialized = true;
        return true;
    }

    /** @brief Shutdown and release resources */
    void Shutdown()
    {
        m_frameHistory.Clear();
        m_initialized = false;
    }

    /**
     * @brief Begin a new frame - call before rendering
     *
     * Records view/projection matrices for motion vector computation
     * and generates the jitter offset for this frame.
     */
    void BeginFrame(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix, float deltaTime)
    {
        m_frameIndex++;

        // Calculate jitter for TAA
        if (m_taaSettings.enabled)
        {
            m_currentJitter = JitterGenerator::GetJitterOffset(m_frameIndex, m_taaSettings.jitterPattern,
                                                               m_taaSettings.jitterSequenceLength);

            // Scale jitter to NDC space
            m_currentJitterNDC = {m_currentJitter.x * 2.0f / static_cast<float>(m_width),
                                  m_currentJitter.y * 2.0f / static_cast<float>(m_height)};
        }
        else
        {
            m_currentJitter = {0.0f, 0.0f};
            m_currentJitterNDC = {0.0f, 0.0f};
        }

        // Store frame data in history
        FrameHistory::FrameData frameData;
        frameData.viewMatrix = viewMatrix;
        frameData.projMatrix = projMatrix;
        frameData.jitterOffset = m_currentJitter;
        frameData.frameIndex = m_frameIndex;
        frameData.deltaTime = deltaTime;

        // Compute viewProj (simplified - in production use XMMatrixMultiply)
        frameData.viewProjMatrix = viewMatrix;    // Placeholder - caller should set
        frameData.invViewProjMatrix = viewMatrix; // Placeholder

        m_frameHistory.PushFrame(frameData);
    }

    /**
     * @brief End the current frame - call after rendering
     *
     * Swaps history buffers and prepares for the next frame.
     */
    void EndFrame()
    {
        // History ping-pong is handled by PushFrame
    }

    /**
     * @brief Update temporal effects simulation
     * @param deltaTime Frame delta time in seconds
     */
    void Update(float deltaTime) { m_totalTime += deltaTime; }

    /** @brief Render/apply temporal effects (post-process pass) */
    void Render()
    {
        // GPU implementation would go here - apply TAA resolve and motion blur
    }

    /**
     * @brief Handle render target resize
     */
    void Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        m_frameHistory.Clear(); // Invalidate history on resize
    }

    // ---- TAA Accessors ----

    /** @brief Get the current frame's jitter offset in pixel space */
    XMFLOAT2 GetJitterOffset() const { return m_currentJitter; }

    /** @brief Get the jitter offset in NDC space (for projection matrix) */
    XMFLOAT2 GetJitterOffsetNDC() const { return m_currentJitterNDC; }

    /** @brief Get the TAA history blend factor */
    float GetHistoryBlendFactor() const
    {
        if (!m_frameHistory.HasSufficientHistory())
            return 0.0f;
        return m_taaSettings.historyBlendFactor;
    }

    /** @brief Check if TAA is active and has history */
    bool IsTAAActive() const { return m_taaSettings.enabled && m_frameHistory.HasSufficientHistory(); }

    // ---- Motion Blur Accessors ----

    /** @brief Check if motion blur is active */
    bool IsMotionBlurActive() const { return m_motionBlurSettings.enabled; }

    /** @brief Get the motion blur sample count */
    int GetMotionBlurSamples() const { return m_motionBlurSettings.sampleCount; }

    /** @brief Get the maximum blur radius in pixels */
    float GetMaxBlurRadius() const { return m_motionBlurSettings.maxBlurRadius; }

    // ---- Settings ----

    TAASettings& GetTAASettings() { return m_taaSettings; }
    const TAASettings& GetTAASettings() const { return m_taaSettings; }

    MotionBlurSettings& GetMotionBlurSettings() { return m_motionBlurSettings; }
    const MotionBlurSettings& GetMotionBlurSettings() const { return m_motionBlurSettings; }

    void SetTAAEnabled(bool enabled) { m_taaSettings.enabled = enabled; }
    void SetMotionBlurEnabled(bool enabled) { m_motionBlurSettings.enabled = enabled; }

    /** @brief Get the frame history for temporal reprojection */
    const FrameHistory& GetFrameHistory() const { return m_frameHistory; }

    /** @brief Get the current frame index */
    uint32_t GetFrameIndex() const { return m_frameIndex; }

    /** @brief Check if the system is initialized */
    bool IsInitialized() const { return m_initialized; }

    // ---- Console Integration ----

    /** @brief Get a summary string of temporal effects state */
    std::string Console_GetStatus() const
    {
        std::string status = "Temporal Effects:\n";
        status += "  TAA: " + std::string(m_taaSettings.enabled ? "ON" : "OFF");
        status += " (blend=" + std::to_string(m_taaSettings.historyBlendFactor) + ")\n";
        status += "  Motion Blur: " + std::string(m_motionBlurSettings.enabled ? "ON" : "OFF");
        status += " (intensity=" + std::to_string(m_motionBlurSettings.intensity) + ")\n";
        status += "  History frames: " + std::to_string(m_frameHistory.GetFrameCount()) + "\n";
        status += "  Frame index: " + std::to_string(m_frameIndex) + "\n";
        return status;
    }

  private:
    bool m_initialized = false;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_frameIndex = 0;
    float m_totalTime = 0.0f;

    // Settings
    TAASettings m_taaSettings;
    MotionBlurSettings m_motionBlurSettings;

    // Frame history
    FrameHistory m_frameHistory;

    // Current frame jitter
    XMFLOAT2 m_currentJitter = {0.0f, 0.0f};
    XMFLOAT2 m_currentJitterNDC = {0.0f, 0.0f};
};
