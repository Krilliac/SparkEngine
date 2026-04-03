/**
 * @file HitchDetector.h
 * @brief Detects frame-time spikes (micro-stutters) above rolling average thresholds
 * @author Spark Engine Team
 * @date 2026
 *
 * Analyzes per-frame delta times against a rolling average to classify hitches
 * by severity (mild, moderate, severe). Tracks hitch frequency, worst-case
 * duration, and per-severity counts.
 *
 * Usage:
 * @code
 *   Spark::HitchDetector::GetInstance().Initialize();
 *   // In main loop:
 *   Spark::HitchDetector::GetInstance().Update(deltaTime);
 *   // At shutdown:
 *   Spark::HitchDetector::GetInstance().Shutdown();
 * @endcode
 *
 * @note Active in Debug and Release builds. Compiled to no-ops in SPARK_SHIPPING.
 * @see FreezeDetector.h, MemoryMonitor.h
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cstdint>

namespace Spark
{

    /**
     * @brief Configuration for frame hitch detection
     */
    struct HitchDetectorConfig
    {
        /// Frame time multiplier thresholds relative to rolling average
        float mildMultiplier = 2.0f;
        float moderateMultiplier = 4.0f;
        float severeMultiplier = 8.0f;

        /// Minimum frame time (ms) to consider — avoids false positives at very high FPS
        float minFrameTimeMs = 4.0f;

        /// Number of frames in the rolling average window
        uint32_t historyWindowSize = 120;

        /// Whether the detector is enabled
        bool enabled = true;
    };

    /**
     * @brief Hitch severity classification
     */
    enum class HitchSeverity : uint8_t
    {
        None,
        Mild,     ///< Frame time >= mildMultiplier * avg
        Moderate, ///< Frame time >= moderateMultiplier * avg
        Severe    ///< Frame time >= severeMultiplier * avg
    };

    /**
     * @brief Snapshot of hitch detector status for diagnostics
     */
    struct HitchDetectorStatus
    {
        float currentAvgFrameTimeMs = 0.0f;
        float worstHitchMs = 0.0f;
        HitchSeverity lastHitchSeverity = HitchSeverity::None;
        uint32_t mildCount = 0;
        uint32_t moderateCount = 0;
        uint32_t severeCount = 0;
        uint32_t totalHitches = 0;
        float hitchesPerMinute = 0.0f;
        uint64_t framesAnalyzed = 0;
    };

    /**
     * @brief Singleton detector for frame-time spikes and micro-stutters
     *
     * Call Update(dt) every frame. The detector maintains a rolling average
     * of frame times and classifies spikes by severity.
     */
    class HitchDetector
    {
      public:
        static HitchDetector& GetInstance();

        void Initialize();
        void Update(float dt);
        void Shutdown();

        void Configure(const HitchDetectorConfig& config);
        [[nodiscard]] const HitchDetectorConfig& GetConfig() const { return m_config; }

        [[nodiscard]] HitchDetectorStatus GetStatus() const;

        /// Reset all accumulated statistics
        void Reset();

        void RegisterConsoleCommands();

      private:
        HitchDetector() = default;
        ~HitchDetector() = default;
        HitchDetector(const HitchDetector&) = delete;
        HitchDetector& operator=(const HitchDetector&) = delete;

        [[nodiscard]] float ComputeRollingAverage() const;

        HitchDetectorConfig m_config;

        static constexpr uint32_t MAX_HISTORY = 300;
        std::array<float, MAX_HISTORY> m_frameTimes{};
        uint32_t m_frameIndex = 0;
        uint32_t m_framesFilled = 0;

        // Statistics
        uint32_t m_mildCount = 0;
        uint32_t m_moderateCount = 0;
        uint32_t m_severeCount = 0;
        float m_worstHitchMs = 0.0f;
        HitchSeverity m_lastHitchSeverity = HitchSeverity::None;
        uint64_t m_framesAnalyzed = 0;

        // Hitches-per-minute tracking
        float m_uptimeSec = 0.0f;
        uint32_t m_hitchesInCurrentMinute = 0;
        float m_currentMinuteElapsed = 0.0f;
        float m_hitchesPerMinute = 0.0f;

        bool m_initialized = false;
    };

} // namespace Spark

#if !defined(SPARK_SHIPPING)
#define SPARK_HITCH_UPDATE(dt) Spark::HitchDetector::GetInstance().Update(dt)
#else
#define SPARK_HITCH_UPDATE(dt) ((void)0)
#endif
