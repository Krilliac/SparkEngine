/**
 * @file DynamicQualityScaler.h
 * @brief Dynamic quality scaling with PID controller and quality cascade
 * @author Spark Engine Team
 * @date 2026
 *
 * Addresses GAP-OFF03: Monitors frame time with running average, auto-adjusts
 * renderScale based on GPU load, steps through quality levels (resolution scale,
 * shadow quality, LOD bias, texture quality, post-processing), and uses a PID
 * controller for smooth scaling.
 */

#pragma once

#include "../Core/Platform.h"
#include "VRAMBudgetMonitor.h"

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
// QUALITY TIER — ORDERED QUALITY REDUCTION STEPS
// ============================================================================

/**
 * @brief Individual quality dimensions that can be scaled independently
 *
 * Listed in the order they should be reduced (first = reduce first):
 *   1. Resolution scale (cheapest visual impact per performance gain)
 *   2. Shadow quality
 *   3. LOD bias
 *   4. Texture quality
 *   5. Post-processing (most visible impact — reduce last)
 */
enum class QualityDimension : uint8_t
{
    ResolutionScale = 0, ///< Render resolution multiplier (0.5 .. 1.0)
    ShadowQuality,       ///< Shadow map resolution and cascade count
    LODBias,             ///< Mesh level-of-detail distance bias
    TextureQuality,      ///< Texture mip bias / quality tier
    PostProcessing,      ///< Optional post-processing effects on/off

    Count
};

/**
 * @brief Convert quality dimension to string
 */
inline const char* QualityDimensionToString(QualityDimension dim)
{
    switch (dim)
    {
    case QualityDimension::ResolutionScale:
        return "ResolutionScale";
    case QualityDimension::ShadowQuality:
        return "ShadowQuality";
    case QualityDimension::LODBias:
        return "LODBias";
    case QualityDimension::TextureQuality:
        return "TextureQuality";
    case QualityDimension::PostProcessing:
        return "PostProcessing";
    default:
        return "Unknown";
    }
}

// ============================================================================
// QUALITY STATE — CURRENT LEVEL PER DIMENSION
// ============================================================================

/**
 * @brief Complete quality state across all scalable dimensions
 */
struct QualityState
{
    /// Resolution scale (0.5 = half res, 1.0 = native)
    float renderScale = 1.0f;

    /// Shadow map resolution multiplier (0.25 = quarter, 1.0 = configured)
    float shadowResolutionScale = 1.0f;

    /// Number of shadow cascades (1..4)
    uint32_t shadowCascadeCount = 4;

    /// LOD distance bias multiplier (1.0 = normal, 0.5 = force closer LOD transitions)
    float lodBias = 1.0f;

    /// Texture mip bias (0 = normal, positive = force lower-res mips)
    float textureMipBias = 0.0f;

    /// Post-processing toggles
    bool bloomEnabled = true;
    bool ssaoEnabled = true;
    bool motionBlurEnabled = true;
    bool ssrEnabled = false;
    bool volumetricsEnabled = false;

    /**
     * @brief Compute an aggregate quality level (0.0 = minimum, 1.0 = maximum)
     */
    float GetAggregateQuality() const
    {
        float quality = 0.0f;
        quality += renderScale;                                   // 0.5 .. 1.0
        quality += shadowResolutionScale;                         // 0.25 .. 1.0
        quality += static_cast<float>(shadowCascadeCount) / 4.0f; // 0.25 .. 1.0
        quality += lodBias;                                       // 0.5 .. 1.0
        quality += (1.0f - textureMipBias / 4.0f);                // 0.0 .. 1.0
        quality += bloomEnabled ? 0.2f : 0.0f;
        quality += ssaoEnabled ? 0.2f : 0.0f;
        quality += motionBlurEnabled ? 0.1f : 0.0f;
        quality += ssrEnabled ? 0.2f : 0.0f;
        quality += volumetricsEnabled ? 0.1f : 0.0f;
        return quality / 5.8f; // Normalize to 0..1 range
    }
};

// ============================================================================
// QUALITY CHANGE CALLBACK
// ============================================================================

/**
 * @brief Callback invoked when the scaler changes quality settings
 * @param newState  The updated quality state to apply
 * @param dimension The dimension that was just changed
 * @param direction +1 if quality was increased, -1 if decreased
 */
using QualityChangeCallback =
    std::function<void(const QualityState& newState, QualityDimension dimension, int direction)>;

// ============================================================================
// PID CONTROLLER
// ============================================================================

/**
 * @brief PID controller for smooth frame-time-based quality adjustment
 *
 * Controls a normalized "quality target" value (0.0 .. 1.0) based on the
 * error between measured frame time and target frame time.
 */
class PIDController
{
  public:
    struct Gains
    {
        float kP = 0.5f;  ///< Proportional gain
        float kI = 0.05f; ///< Integral gain
        float kD = 0.1f;  ///< Derivative gain
    };

    PIDController() = default;

    explicit PIDController(const Gains& gains) : m_gains(gains) {}

    /**
     * @brief Compute the PID output for the current frame
     *
     * @param error       Current error (targetFrameTime - measuredFrameTime).
     *                    Positive = GPU has headroom, negative = GPU is over budget.
     * @param deltaTime   Time since last update (seconds)
     * @return Control signal in range [-1, 1]. Negative = reduce quality, positive = increase.
     */
    float Update(float error, float deltaTime)
    {
        if (deltaTime <= 0.0f)
        {
            return 0.0f;
        }

        // Proportional term
        float pTerm = m_gains.kP * error;

        // Integral term with anti-windup clamping
        m_integral += error * deltaTime;
        m_integral = std::clamp(m_integral, -m_integralLimit, m_integralLimit);
        float iTerm = m_gains.kI * m_integral;

        // Derivative term (on error, not measurement, for simplicity)
        float derivative = (error - m_previousError) / deltaTime;
        float dTerm = m_gains.kD * derivative;

        m_previousError = error;

        // Clamp output
        float output = pTerm + iTerm + dTerm;
        return std::clamp(output, -1.0f, 1.0f);
    }

    /**
     * @brief Reset controller state
     */
    void Reset()
    {
        m_integral = 0.0f;
        m_previousError = 0.0f;
    }

    /**
     * @brief Set PID gains
     */
    void SetGains(const Gains& gains) { m_gains = gains; }
    Gains GetGains() const { return m_gains; }

    /**
     * @brief Set integral windup limit
     */
    void SetIntegralLimit(float limit) { m_integralLimit = limit; }

  private:
    Gains m_gains;
    float m_integral = 0.0f;
    float m_previousError = 0.0f;
    float m_integralLimit = 10.0f;
};

// ============================================================================
// DYNAMIC QUALITY SCALER
// ============================================================================

/**
 * @brief Dynamically adjusts rendering quality to maintain target frame rate
 *
 * Uses a PID controller driven by GPU frame time to smoothly adjust quality
 * dimensions in a priority-ordered cascade. Integrates with memory pressure
 * events from VRAMBudgetMonitor for VRAM-driven quality reduction.
 *
 * Thread safety: All public methods are thread-safe. Call Update() from the
 * main render thread each frame.
 */
class DynamicQualityScaler
{
  public:
    // ========================================================================
    // CONFIGURATION
    // ========================================================================

    struct Config
    {
        /// Target frame time in milliseconds (16.67ms = 60fps, 33.33ms = 30fps)
        float targetFrameTimeMs = 16.67f;

        /// Resolution scale limits
        float minRenderScale = 0.5f;
        float maxRenderScale = 1.0f;

        /// Resolution scale step size per adjustment
        float renderScaleStep = 0.05f;

        /// Shadow resolution scale limits
        float minShadowScale = 0.25f;
        float maxShadowScale = 1.0f;
        float shadowScaleStep = 0.25f;

        /// LOD bias limits
        float minLodBias = 0.5f;
        float maxLodBias = 1.0f;
        float lodBiasStep = 0.1f;

        /// Texture mip bias limits (0 = highest quality, 4 = lowest)
        float minTextureMipBias = 0.0f;
        float maxTextureMipBias = 4.0f;
        float textureMipBiasStep = 1.0f;

        /// Number of frames to average for frame time measurement
        uint32_t frameTimeWindowSize = 30;

        /// Minimum interval between quality changes (frames)
        uint32_t minChangeIntervalFrames = 15;

        /// Hysteresis: GPU must be under target by this margin (ms) before increasing quality
        float increaseThresholdMs = 2.0f;

        /// PID controller gains
        PIDController::Gains pidGains = {0.5f, 0.05f, 0.1f};

        /// Enable the scaler
        bool enabled = true;
    };

    // ========================================================================
    // CONSTRUCTION
    // ========================================================================

    DynamicQualityScaler() = default;
    ~DynamicQualityScaler() = default;

    // Non-copyable
    DynamicQualityScaler(const DynamicQualityScaler&) = delete;
    DynamicQualityScaler& operator=(const DynamicQualityScaler&) = delete;

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    /**
     * @brief Initialize the scaler with default config
     */
    void Initialize()
    {
        Config defaultConfig;
        Initialize(defaultConfig, nullptr);
    }

    /**
     * @brief Initialize the scaler
     * @param config         Configuration
     * @param budgetMonitor  Optional VRAM budget monitor for memory-driven scaling
     */
    void Initialize(const Config& config, VRAMBudgetMonitor* budgetMonitor = nullptr)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_config = config;
        m_budgetMonitor = budgetMonitor;
        m_pid = PIDController(config.pidGains);
        m_initialized = true;

        // Register for memory pressure events
        if (m_budgetMonitor)
        {
            m_pressureListenerId = m_budgetMonitor->AddPressureListener(
                "DynamicQualityScaler", [this](MemoryPressureLevel level, const GPUMemoryInfo& /*info*/)
                { m_pendingPressureLevel.store(static_cast<int>(level), std::memory_order_release); });
        }
    }

    /**
     * @brief Shut down the scaler
     */
    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_budgetMonitor && m_pressureListenerId != 0)
        {
            m_budgetMonitor->RemovePressureListener(m_pressureListenerId);
            m_pressureListenerId = 0;
        }

        m_initialized = false;
    }

    // ========================================================================
    // PER-FRAME UPDATE
    // ========================================================================

    /**
     * @brief Update the scaler with the current frame's GPU time
     *
     * Call once per frame from the main render thread after EndFrame().
     *
     * @param gpuFrameTimeMs  Measured GPU frame time in milliseconds
     * @param deltaTimeS      Wall-clock delta time in seconds (for PID)
     */
    void Update(float gpuFrameTimeMs, float deltaTimeS)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized || !m_config.enabled)
        {
            return;
        }

        // Add to frame time history
        m_frameTimeHistory.push_back(gpuFrameTimeMs);
        while (m_frameTimeHistory.size() > m_config.frameTimeWindowSize)
        {
            m_frameTimeHistory.pop_front();
        }

        m_framesSinceLastChange++;

        // Compute running average
        float avgFrameTime = ComputeAverageFrameTime();
        m_currentAverageFrameTimeMs = avgFrameTime;

        // Handle memory pressure events
        HandleMemoryPressure();

        // Check if we've waited long enough since last change
        if (m_framesSinceLastChange < m_config.minChangeIntervalFrames)
        {
            return;
        }

        // Compute PID error: positive = headroom, negative = over budget
        float error = m_config.targetFrameTimeMs - avgFrameTime;
        float pidOutput = m_pid.Update(error, deltaTimeS);

        // Decide whether to adjust quality
        if (pidOutput < -0.1f)
        {
            // GPU is over budget — reduce quality
            ReduceQuality();
        }
        else if (pidOutput > 0.1f && error > m_config.increaseThresholdMs)
        {
            // GPU has significant headroom — increase quality
            IncreaseQuality();
        }
    }

    // ========================================================================
    // QUALITY STATE
    // ========================================================================

    /**
     * @brief Get the current quality state
     */
    QualityState GetQualityState() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_qualityState;
    }

    /**
     * @brief Set the quality state directly (e.g., from user settings)
     */
    void SetQualityState(const QualityState& state)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_qualityState = state;
        m_pid.Reset();
    }

    /**
     * @brief Reset quality to maximum
     */
    void ResetToMaxQuality()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_qualityState.renderScale = m_config.maxRenderScale;
        m_qualityState.shadowResolutionScale = m_config.maxShadowScale;
        m_qualityState.shadowCascadeCount = 4;
        m_qualityState.lodBias = m_config.maxLodBias;
        m_qualityState.textureMipBias = m_config.minTextureMipBias;
        m_qualityState.bloomEnabled = true;
        m_qualityState.ssaoEnabled = true;
        m_qualityState.motionBlurEnabled = true;

        m_currentCascadeStep = 0;
        m_pid.Reset();
        m_frameTimeHistory.clear();
    }

    // ========================================================================
    // CALLBACKS
    // ========================================================================

    /**
     * @brief Register a callback for quality changes
     * @return Callback ID for later removal
     */
    uint32_t AddChangeCallback(QualityChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32_t id = m_nextCallbackId++;
        m_callbacks.emplace_back(id, std::move(callback));
        return id;
    }

    /**
     * @brief Remove a quality change callback
     */
    void RemoveChangeCallback(uint32_t callbackId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_callbacks.erase(std::remove_if(m_callbacks.begin(), m_callbacks.end(),
                                         [callbackId](const auto& pair) { return pair.first == callbackId; }),
                          m_callbacks.end());
    }

    // ========================================================================
    // DIAGNOSTICS
    // ========================================================================

    /**
     * @brief Get the current averaged GPU frame time
     */
    float GetAverageFrameTimeMs() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentAverageFrameTimeMs;
    }

    /**
     * @brief Get the target frame time
     */
    float GetTargetFrameTimeMs() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.targetFrameTimeMs;
    }

    /**
     * @brief Check if the scaler is enabled
     */
    bool IsEnabled() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.enabled;
    }

    /**
     * @brief Enable or disable the scaler at runtime
     */
    void SetEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.enabled = enabled;
        if (!enabled)
        {
            m_pid.Reset();
        }
    }

    /**
     * @brief Get the current configuration
     */
    Config GetConfig() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    /**
     * @brief Update configuration at runtime
     */
    void SetConfig(const Config& config)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        m_pid.SetGains(config.pidGains);
    }

    /**
     * @brief Get the current cascade step index (which quality tier is being adjusted)
     */
    uint32_t GetCurrentCascadeStep() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentCascadeStep;
    }

  private:
    // ========================================================================
    // QUALITY CASCADE — REDUCE
    // ========================================================================

    /**
     * @brief Reduce quality by stepping through the cascade
     *
     * Order: resolution scale -> shadow quality -> LOD bias -> texture quality -> post-processing
     */
    void ReduceQuality()
    {
        bool changed = false;
        QualityDimension changedDimension = QualityDimension::ResolutionScale;

        // Step 1: Reduce render scale
        if (m_qualityState.renderScale > m_config.minRenderScale)
        {
            m_qualityState.renderScale =
                std::max(m_config.minRenderScale, m_qualityState.renderScale - m_config.renderScaleStep);
            changed = true;
            changedDimension = QualityDimension::ResolutionScale;
        }
        // Step 2: Reduce shadow quality
        else if (m_qualityState.shadowResolutionScale > m_config.minShadowScale)
        {
            m_qualityState.shadowResolutionScale =
                std::max(m_config.minShadowScale, m_qualityState.shadowResolutionScale - m_config.shadowScaleStep);
            changed = true;
            changedDimension = QualityDimension::ShadowQuality;
        }
        else if (m_qualityState.shadowCascadeCount > 1)
        {
            m_qualityState.shadowCascadeCount--;
            changed = true;
            changedDimension = QualityDimension::ShadowQuality;
        }
        // Step 3: Reduce LOD bias
        else if (m_qualityState.lodBias > m_config.minLodBias)
        {
            m_qualityState.lodBias = std::max(m_config.minLodBias, m_qualityState.lodBias - m_config.lodBiasStep);
            changed = true;
            changedDimension = QualityDimension::LODBias;
        }
        // Step 4: Increase texture mip bias (lower quality)
        else if (m_qualityState.textureMipBias < m_config.maxTextureMipBias)
        {
            m_qualityState.textureMipBias =
                std::min(m_config.maxTextureMipBias, m_qualityState.textureMipBias + m_config.textureMipBiasStep);
            changed = true;
            changedDimension = QualityDimension::TextureQuality;
        }
        // Step 5: Disable post-processing effects
        else if (m_qualityState.motionBlurEnabled)
        {
            m_qualityState.motionBlurEnabled = false;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }
        else if (m_qualityState.ssaoEnabled)
        {
            m_qualityState.ssaoEnabled = false;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }
        else if (m_qualityState.bloomEnabled)
        {
            m_qualityState.bloomEnabled = false;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }

        if (changed)
        {
            m_framesSinceLastChange = 0;
            m_currentCascadeStep++;
            NotifyCallbacks(changedDimension, -1);
        }
    }

    // ========================================================================
    // QUALITY CASCADE — INCREASE
    // ========================================================================

    /**
     * @brief Increase quality in reverse cascade order
     *
     * Order: post-processing -> texture quality -> LOD bias -> shadow quality -> resolution scale
     */
    void IncreaseQuality()
    {
        bool changed = false;
        QualityDimension changedDimension = QualityDimension::ResolutionScale;

        // Step 1: Re-enable post-processing effects
        if (!m_qualityState.bloomEnabled)
        {
            m_qualityState.bloomEnabled = true;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }
        else if (!m_qualityState.ssaoEnabled)
        {
            m_qualityState.ssaoEnabled = true;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }
        else if (!m_qualityState.motionBlurEnabled)
        {
            m_qualityState.motionBlurEnabled = true;
            changed = true;
            changedDimension = QualityDimension::PostProcessing;
        }
        // Step 2: Decrease texture mip bias (higher quality)
        else if (m_qualityState.textureMipBias > m_config.minTextureMipBias)
        {
            m_qualityState.textureMipBias =
                std::max(m_config.minTextureMipBias, m_qualityState.textureMipBias - m_config.textureMipBiasStep);
            changed = true;
            changedDimension = QualityDimension::TextureQuality;
        }
        // Step 3: Increase LOD bias
        else if (m_qualityState.lodBias < m_config.maxLodBias)
        {
            m_qualityState.lodBias = std::min(m_config.maxLodBias, m_qualityState.lodBias + m_config.lodBiasStep);
            changed = true;
            changedDimension = QualityDimension::LODBias;
        }
        // Step 4: Increase shadow quality
        else if (m_qualityState.shadowCascadeCount < 4)
        {
            m_qualityState.shadowCascadeCount++;
            changed = true;
            changedDimension = QualityDimension::ShadowQuality;
        }
        else if (m_qualityState.shadowResolutionScale < m_config.maxShadowScale)
        {
            m_qualityState.shadowResolutionScale =
                std::min(m_config.maxShadowScale, m_qualityState.shadowResolutionScale + m_config.shadowScaleStep);
            changed = true;
            changedDimension = QualityDimension::ShadowQuality;
        }
        // Step 5: Increase render scale
        else if (m_qualityState.renderScale < m_config.maxRenderScale)
        {
            m_qualityState.renderScale =
                std::min(m_config.maxRenderScale, m_qualityState.renderScale + m_config.renderScaleStep);
            changed = true;
            changedDimension = QualityDimension::ResolutionScale;
        }

        if (changed)
        {
            m_framesSinceLastChange = 0;
            if (m_currentCascadeStep > 0)
            {
                m_currentCascadeStep--;
            }
            NotifyCallbacks(changedDimension, +1);
        }
    }

    // ========================================================================
    // MEMORY PRESSURE HANDLING
    // ========================================================================

    /**
     * @brief Handle deferred memory pressure events
     *
     * Memory pressure can trigger immediate quality reduction independent
     * of frame time, focusing on VRAM-heavy dimensions (textures, shadows).
     */
    void HandleMemoryPressure()
    {
        int pressureInt = m_pendingPressureLevel.exchange(-1, std::memory_order_acquire);
        if (pressureInt < 0)
        {
            return;
        }

        auto level = static_cast<MemoryPressureLevel>(pressureInt);

        switch (level)
        {
        case MemoryPressureLevel::Warning:
            // Increase texture mip bias by 1 step
            if (m_qualityState.textureMipBias < m_config.maxTextureMipBias)
            {
                m_qualityState.textureMipBias =
                    std::min(m_config.maxTextureMipBias, m_qualityState.textureMipBias + m_config.textureMipBiasStep);
                NotifyCallbacks(QualityDimension::TextureQuality, -1);
            }
            break;

        case MemoryPressureLevel::Critical:
            // Reduce shadow resolution
            if (m_qualityState.shadowResolutionScale > m_config.minShadowScale)
            {
                m_qualityState.shadowResolutionScale =
                    std::max(m_config.minShadowScale, m_qualityState.shadowResolutionScale - m_config.shadowScaleStep);
                NotifyCallbacks(QualityDimension::ShadowQuality, -1);
            }
            // Also increase texture mip bias
            if (m_qualityState.textureMipBias < m_config.maxTextureMipBias)
            {
                m_qualityState.textureMipBias =
                    std::min(m_config.maxTextureMipBias, m_qualityState.textureMipBias + m_config.textureMipBiasStep);
                NotifyCallbacks(QualityDimension::TextureQuality, -1);
            }
            break;

        case MemoryPressureLevel::Emergency:
            // Aggressive reduction: shadows to minimum, textures to lowest, drop cascades
            m_qualityState.shadowResolutionScale = m_config.minShadowScale;
            m_qualityState.shadowCascadeCount = 1;
            m_qualityState.textureMipBias = m_config.maxTextureMipBias;
            NotifyCallbacks(QualityDimension::ShadowQuality, -1);
            NotifyCallbacks(QualityDimension::TextureQuality, -1);

            // Also reduce render scale
            if (m_qualityState.renderScale > m_config.minRenderScale)
            {
                m_qualityState.renderScale = m_config.minRenderScale;
                NotifyCallbacks(QualityDimension::ResolutionScale, -1);
            }
            break;

        case MemoryPressureLevel::None:
        default:
            break;
        }
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    /**
     * @brief Compute the running average of recent frame times
     */
    float ComputeAverageFrameTime() const
    {
        if (m_frameTimeHistory.empty())
        {
            return m_config.targetFrameTimeMs;
        }

        float sum = 0.0f;
        for (float t : m_frameTimeHistory)
        {
            sum += t;
        }
        return sum / static_cast<float>(m_frameTimeHistory.size());
    }

    /**
     * @brief Notify all registered callbacks of a quality change
     */
    void NotifyCallbacks(QualityDimension dimension, int direction)
    {
        for (const auto& [id, callback] : m_callbacks)
        {
            callback(m_qualityState, dimension, direction);
        }
    }

    // ========================================================================
    // MEMBER DATA
    // ========================================================================

    mutable std::mutex m_mutex;
    bool m_initialized = false;

    Config m_config;
    QualityState m_qualityState;
    PIDController m_pid;

    /// Frame time measurement
    std::deque<float> m_frameTimeHistory;
    float m_currentAverageFrameTimeMs = 0.0f;

    /// Change rate limiting
    uint32_t m_framesSinceLastChange = 0;
    uint32_t m_currentCascadeStep = 0;

    /// VRAM pressure integration
    VRAMBudgetMonitor* m_budgetMonitor = nullptr;
    uint32_t m_pressureListenerId = 0;
    std::atomic<int> m_pendingPressureLevel{-1};

    /// Quality change callbacks
    std::vector<std::pair<uint32_t, QualityChangeCallback>> m_callbacks;
    uint32_t m_nextCallbackId = 1;
};
