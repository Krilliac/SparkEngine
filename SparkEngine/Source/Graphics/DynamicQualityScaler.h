/**
 * @file DynamicQualityScaler.h
 * @brief Dynamic resolution scaler that adjusts render resolution based on frame performance
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks FPS over a sliding window and adjusts the render resolution scale
 * to maintain a target frame rate. Uses the types defined in DynamicQualityTypes.h.
 *
 * @see DynamicQualityTypes.h, UpscalingSettings
 */

#pragma once

#include "DynamicQualityTypes.h"

#include <array>
#include <cstdint>

namespace Spark::Graphics
{

    /// @brief Maximum number of frame samples in the sliding window
    static constexpr int DQS_WINDOW_SIZE = 64;

    /// @brief Thresholds for quality recommendation
    struct DynamicQualityThresholds
    {
        float targetFPS = 60.0f;           ///< Desired frame rate
        float headroomPercent = 10.0f;     ///< FPS headroom before upgrading quality (%)
        float dropThresholdPercent = 5.0f; ///< FPS deficit before downgrading quality (%)
        float minScale = 0.33f;            ///< Minimum render resolution scale
        float maxScale = 1.0f;             ///< Maximum render resolution scale
        float scaleStepDown = 0.05f;       ///< Scale reduction per adjustment step
        float scaleStepUp = 0.02f;         ///< Scale increase per adjustment step (slower)
        float adjustCooldownSec = 0.5f;    ///< Minimum time between adjustments
    };

    /**
     * @brief Tracks frame timing and adjusts render resolution scale dynamically
     *
     * Call RecordFrameTime() each frame with the delta time. The scaler maintains
     * a sliding window of frame times and computes average FPS. When average FPS
     * drops below the target, it reduces the resolution scale. When FPS is above
     * the target with headroom, it increases the scale.
     */
    class DynamicQualityScaler
    {
      public:
        /** @brief Singleton access */
        static DynamicQualityScaler& GetInstance()
        {
            static DynamicQualityScaler s;
            return s;
        }

        ~DynamicQualityScaler() = default;

      private:
        DynamicQualityScaler() = default;
        DynamicQualityScaler(const DynamicQualityScaler&) = delete;
        DynamicQualityScaler& operator=(const DynamicQualityScaler&) = delete;

      public:
        /// @brief Initialize with thresholds
        void Initialize(const DynamicQualityThresholds& thresholds);

        /// @brief Record a frame's delta time (seconds) and update the scale
        void RecordFrameTime(float deltaTimeSec);

        /// @brief Get the current render resolution scale [minScale, maxScale]
        float GetCurrentResolutionScale() const;

        /// @brief Get the recommended UpscalingQuality based on current scale
        UpscalingQuality GetRecommendedQuality() const;

        /// @brief Get the average FPS over the sliding window
        float GetAverageFPS() const;

        /// @brief Get the average frame time in seconds
        float GetAverageFrameTime() const;

        /// @brief Reset the scaler state (e.g., on scene change)
        void Reset();

        /// @brief Check if the scaler has enough data for a valid reading
        bool HasValidData() const;

        /// @brief Access the thresholds
        const DynamicQualityThresholds& GetThresholds() const;

        /**
         * @brief Compute scaled render dimensions from native resolution
         * @param nativeWidth Full resolution width
         * @param nativeHeight Full resolution height
         * @param outWidth Scaled render width (clamped to minimum 1)
         * @param outHeight Scaled render height (clamped to minimum 1)
         */
        void GetRenderDimensions(uint32_t nativeWidth, uint32_t nativeHeight, uint32_t& outWidth,
                                 uint32_t& outHeight) const;

      private:
        DynamicQualityThresholds m_thresholds;
        std::array<float, DQS_WINDOW_SIZE> m_frameTimes{};
        int m_writeIndex = 0;
        int m_sampleCount = 0;
        float m_currentScale = 1.0f;
        float m_timeSinceLastAdjust = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
